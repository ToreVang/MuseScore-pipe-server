"""
MuseScore pipe-server client
============================
Keeps a single MuseScore process alive and converts MusicXML (or any supported
format) to images by sending newline-delimited JSON requests over stdin/stdout.

This eliminates the ~0.6 s startup overhead for every conversion: you pay the
cost once, then each convert() call only runs the actual parsing + rendering.

Usage
-----
    from musescore_pipe_client import MuseScoreClient

    with MuseScoreClient("/path/to/mscore") as ms:
        ms.convert("score.musicxml", "out.png", dpi=150, trim=0)
        ms.convert("score2.musicxml", "out2.png", dpi=200)

Protocol (implemented in src/app/internal/consoleapp.cpp)
----------------------------------------------------------
  Request  (stdin, one JSON object per line):
      {"in": "<input path>", "out": "<output path>", "dpi": <float>, "trim": <int>}

  Response (stdout, one JSON object per line):
      {"status": "ready"}          — sent once after startup
      {"status": "ok"}             — conversion succeeded
      {"status": "error", "message": "..."} — conversion failed
"""

import json
import os
import subprocess
import threading
from pathlib import Path


class MuseScoreClient:
    """Persistent MuseScore process for fast repeated conversions."""

    def __init__(self, musescore_path: str, extra_args: list[str] | None = None):
        """
        Parameters
        ----------
        musescore_path:
            Path to the MuseScore executable.
        extra_args:
            Optional extra CLI arguments forwarded to MuseScore (e.g.
            ``["--musicxml-use-default-font"]``).
        """
        self._musescore_path = musescore_path
        self._extra_args = extra_args or []
        self._process: subprocess.Popen | None = None
        self._lock = threading.Lock()

    # ------------------------------------------------------------------
    # Context-manager support
    # ------------------------------------------------------------------

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *_):
        self.stop()

    # ------------------------------------------------------------------
    # Lifecycle
    # ------------------------------------------------------------------

    def start(self):
        """Start the MuseScore server process and wait until it is ready."""
        if self._process is not None:
            return

        env = os.environ.copy()
        # Force offscreen rendering — no display needed for image export
        env.setdefault("QT_QPA_PLATFORM", "offscreen")

        cmd = [self._musescore_path, "--pipe-server"] + self._extra_args
        self._process = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            env=env,
        )

        # Block until MuseScore signals {"status": "ready"}
        ready_line = self._readline()
        msg = json.loads(ready_line)
        if msg.get("status") != "ready":
            raise RuntimeError(f"MuseScore pipe-server did not start correctly: {msg}")

    def stop(self):
        """Shut down the MuseScore process cleanly."""
        if self._process is None:
            return
        try:
            self._process.stdin.close()  # closing stdin causes MuseScore to exit(0)
            self._process.wait(timeout=5)
        except Exception:
            self._process.kill()
        finally:
            self._process = None

    @property
    def is_running(self) -> bool:
        return self._process is not None and self._process.poll() is None

    # ------------------------------------------------------------------
    # Conversion
    # ------------------------------------------------------------------

    def convert(
        self,
        input_path: str | Path,
        output_path: str | Path,
        *,
        dpi: float | None = None,
        trim: int | None = None,
    ) -> None:
        """
        Convert *input_path* to *output_path* using the running MuseScore process.

        Parameters
        ----------
        input_path:
            Source file (MusicXML, MSCZ, …).
        output_path:
            Destination file (PNG, SVG, PDF, …).
        dpi:
            Image resolution override (PNG/SVG exports).
        trim:
            Trim-margin size in pixels (0 = no trim).

        Raises
        ------
        RuntimeError
            If MuseScore reports a conversion error.
        """
        if not self.is_running:
            raise RuntimeError("MuseScore pipe-server is not running. Call start() first.")

        request: dict = {
            "in": str(input_path),
            "out": str(output_path),
        }
        if dpi is not None:
            request["dpi"] = dpi
        if trim is not None:
            request["trim"] = trim

        with self._lock:
            line = json.dumps(request) + "\n"
            self._process.stdin.write(line.encode())
            self._process.stdin.flush()
            response_line = self._readline()

        response = json.loads(response_line)
        if response.get("status") != "ok":
            raise RuntimeError(
                f"MuseScore conversion failed: {response.get('message', response)}"
            )

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    def _readline(self) -> str:
        """Read one line from MuseScore stdout, raising if the process died."""
        line = self._process.stdout.readline()
        if not line:
            rc = self._process.wait()
            raise RuntimeError(f"MuseScore process exited unexpectedly (code {rc})")
        return line.decode().strip()


# ---------------------------------------------------------------------------
# Convenience one-shot function (no persistent process)
# ---------------------------------------------------------------------------

def convert_once(
    musescore_path: str,
    input_path: str | Path,
    output_path: str | Path,
    *,
    dpi: float | None = None,
    trim: int | None = None,
) -> None:
    """
    Convert a single file without keeping a persistent process.

    Equivalent to the original subprocess.run() call — useful when you only
    need one conversion and don't want to manage process lifetime.
    """
    with MuseScoreClient(musescore_path) as ms:
        ms.convert(input_path, output_path, dpi=dpi, trim=trim)


# ---------------------------------------------------------------------------
# Quick smoke-test / demo
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    import sys
    import time

    if len(sys.argv) < 4:
        print(
            "Usage: python musescore_pipe_client.py <mscore_bin> <input.musicxml> <output.png> [dpi]"
        )
        sys.exit(1)

    mscore_bin = sys.argv[1]
    in_file = sys.argv[2]
    out_file = sys.argv[3]
    resolution = float(sys.argv[4]) if len(sys.argv) > 4 else 150.0

    print(f"Starting MuseScore pipe-server: {mscore_bin}")
    t0 = time.perf_counter()

    with MuseScoreClient(mscore_bin) as ms:
        startup = time.perf_counter() - t0
        print(f"  Server ready in {startup:.3f}s")

        # First conversion
        t1 = time.perf_counter()
        ms.convert(in_file, out_file, dpi=resolution, trim=0)
        print(f"  Conversion 1: {time.perf_counter() - t1:.3f}s → {out_file}")

        # Second conversion (same file, shows repeated-call speed)
        out2 = out_file.replace(".png", "_2.png") if out_file.endswith(".png") else out_file + "_2"
        t2 = time.perf_counter()
        ms.convert(in_file, out2, dpi=resolution, trim=0)
        print(f"  Conversion 2: {time.perf_counter() - t2:.3f}s → {out2}")
