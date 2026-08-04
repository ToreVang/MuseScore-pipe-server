/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "consoleapp.h"

#ifndef Q_OS_WIN
#include <unistd.h>
#else
#include <io.h>
#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif
using ssize_t = int;
static inline ssize_t posix_read(int fd, void* buf, size_t count) { return ::_read(fd, buf, static_cast<unsigned>(count)); }
#define posix_read_defined
#endif
#ifndef posix_read_defined
static inline ssize_t posix_read(int fd, void* buf, size_t count) { return ::read(fd, buf, count); }
#endif

#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#ifndef Q_OS_WASM
#include <QThreadPool>
#endif

#include "modularity/ioc.h"
#include "async/processevents.h"

#include "muse_framework_config.h"
#include "app_config.h"

#include "log.h"
#include "settings.h"

using namespace muse;
using namespace mu::app;
using namespace mu::converter;

static std::optional<ConvertTarget> parseTarget(const QMap<CmdOptions::ParamKey, QVariant>& params)
{
    auto it = params.find(CmdOptions::ParamKey::ScoreRegion);
    if (it != params.end()) {
        return it.value().toString().toStdString();
    }

    it = params.find(CmdOptions::ParamKey::PageNumber);
    if (it == params.end()) {
        return std::nullopt;
    }

    bool ok = true;
    page_num_t num = it.value().toULongLong(&ok) - 1;

    if (!ok) {
        LOGE() << "Invalid page, ignoring...";
        return std::nullopt;
    }

    return num;
}

ConsoleApp::ConsoleApp(const CmdOptions& options, const modularity::ContextPtr& ctx)
    : muse::BaseApplication(ctx), m_options(options)
{
}

void ConsoleApp::addModule(modularity::IModuleSetup* module)
{
    m_modules.push_back(module);
}

void ConsoleApp::setup()
{
    const CmdOptions& options = m_options;

    IApplication::RunMode runMode = options.runMode;
    setRunMode(runMode);

    // ====================================================
    // Setup modules: Resources, Exports, Imports, UiTypes
    // ====================================================
    m_globalModule.setApplication(shared_from_this());
    m_globalModule.registerResources();
    m_globalModule.registerExports();

    for (modularity::IModuleSetup* m : m_modules) {
        m->setApplication(shared_from_this());
        m->registerResources();
    }

    for (modularity::IModuleSetup* m : m_modules) {
        m->registerExports();
    }

#ifndef MUSE_MULTICONTEXT_WIP
    modularity::ContextPtr ctx = std::make_shared<modularity::Context>();
    ctx->id = 0;
    std::vector<muse::modularity::IContextSetup*>& csetups = contextSetups(ctx);
    for (modularity::IContextSetup* s : csetups) {
        s->registerExports();
    }
#endif

    m_globalModule.resolveImports();
    for (modularity::IModuleSetup* m : m_modules) {
        m->resolveImports();
    }

#ifndef MUSE_MULTICONTEXT_WIP
    for (modularity::IContextSetup* s : csetups) {
        s->resolveImports();
    }
#endif

    m_globalModule.registerApi();
    for (modularity::IModuleSetup* m : m_modules) {
        m->registerApi();
    }

    // ====================================================
    // Setup modules: apply the command line options
    // ====================================================
    applyCommandLineOptions(options, runMode);

    // ====================================================
    // Setup modules: onPreInit
    // ====================================================
    m_globalModule.onPreInit(runMode);
    for (modularity::IModuleSetup* m : m_modules) {
        m->onPreInit(runMode);
    }

#ifndef MUSE_MULTICONTEXT_WIP
    for (modularity::IContextSetup* s : csetups) {
        s->onPreInit(runMode);
    }
#endif
    // ====================================================
    // Setup modules: onInit
    // ====================================================
    m_globalModule.onInit(runMode);
    for (modularity::IModuleSetup* m : m_modules) {
        m->onInit(runMode);
    }

#ifndef MUSE_MULTICONTEXT_WIP
    for (modularity::IContextSetup* s : csetups) {
        s->onInit(runMode);
    }
#endif
    // ====================================================
    // Setup modules: onAllInited
    // ====================================================
    m_globalModule.onAllInited(runMode);
    for (modularity::IModuleSetup* m : m_modules) {
        m->onAllInited(runMode);
    }

#ifndef MUSE_MULTICONTEXT_WIP
    for (modularity::IContextSetup* s : csetups) {
        s->onAllInited(runMode);
    }
#endif

    // ====================================================
    // Setup modules: onStartApp (on next event loop)
    // ====================================================
    QMetaObject::invokeMethod(qApp, [this]() {
        m_globalModule.onStartApp();
        for (modularity::IModuleSetup* m : m_modules) {
            m->onStartApp();
        }
    }, Qt::QueuedConnection);

    // ====================================================
    // Run
    // ====================================================

    switch (runMode) {
    case IApplication::RunMode::ConsoleApp: {
        // ====================================================
        // Process Autobot
        // ====================================================
        CmdOptions::Autobot autobotOptions = options.autobot;
        if (!autobotOptions.testCaseNameOrFile.isEmpty()) {
            QMetaObject::invokeMethod(qApp, [this, autobotOptions]() {
                    processAutobot(autobotOptions);
                }, Qt::QueuedConnection);
        } else {
            // ====================================================
            // Process Diagnostic
            // ====================================================
            CmdOptions::Diagnostic diagnostic = options.diagnostic;
            if (diagnostic.type != DiagnosticType::Undefined) {
                QMetaObject::invokeMethod(qApp, [this, diagnostic]() {
                        int code = processDiagnostic(diagnostic);
                        qApp->exit(code);
                    }, Qt::QueuedConnection);
            } else {
                // ====================================================
                // Process Converter
                // ====================================================
                CmdOptions::ConverterTask task = options.converterTask;
                if (task.type == ConvertType::PipeServer) {
                    QMetaObject::invokeMethod(qApp, [this]() {
                            processPipeServer();
                        }, Qt::QueuedConnection);
                } else {
                    QMetaObject::invokeMethod(qApp, [this, task]() {
                            int code = processConverter(task);
                            qApp->exit(code);
                        }, Qt::QueuedConnection);
                }
            }
        }
    } break;
    case IApplication::RunMode::AudioPluginRegistration: {
        CmdOptions::AudioPluginRegistration pluginRegistration = options.audioPluginRegistration;

        QMetaObject::invokeMethod(qApp, [this, pluginRegistration]() {
                int code = processAudioPluginRegistration(pluginRegistration);
                qApp->exit(code);
            }, Qt::QueuedConnection);
    } break;
    default: {
        UNREACHABLE;
    }
    }
}

int ConsoleApp::contextCount() const
{
    return m_context ? 1 : 0;
}

std::vector<muse::modularity::ContextPtr> ConsoleApp::contexts() const
{
    return { m_context };
}

std::vector<muse::modularity::IContextSetup*>& ConsoleApp::contextSetups(
    const muse::modularity::ContextPtr& ctx)
{
    for (Context& c : m_contexts) {
        if (c.ctx->id == ctx->id) {
            return c.setups;
        }
    }

    m_contexts.emplace_back();

    Context& ref = m_contexts.back();
    ref.ctx = ctx;

    modularity::IContextSetup* global = m_globalModule.newContext(ctx);
    if (global) {
        ref.setups.push_back(global);
    }

    for (modularity::IModuleSetup* m : m_modules) {
        modularity::IContextSetup* s = m->newContext(ctx);
        if (s) {
            ref.setups.push_back(s);
        }
    }

    return ref.setups;
}

muse::modularity::ContextPtr ConsoleApp::setupNewContext()
{
    //! NOTE
    //! We're currently in a transitional state from a single global context to multiple contexts.
    //! Therefore, this code will be improved; not everything is yet complete,
    //! for example, there's no way to delete (close) a specific context.
    //! Probably the context initialization needs to be moved to the base class of the app.

    m_context = std::make_shared<modularity::Context>();
    auto& ctx = m_context;
    // only global
    ctx->id = 0;

    const CmdOptions& options = m_options;
    IApplication::RunMode runMode = options.runMode;
    if (runMode == IApplication::RunMode::AudioPluginRegistration) {
        return nullptr;
    }

    IF_ASSERT_FAILED(runMode == IApplication::RunMode::ConsoleApp) {
        return nullptr;
    }

    LOGI() << "New context created with id: " << ctx->id;

    // Setup
#ifdef MUSE_MULTICONTEXT_WIP
    std::vector<muse::modularity::IContextSetup*>& csetups = contextSetups(ctx);

    for (modularity::IContextSetup* s : contexts) {
        s->registerExports();
    }

    for (modularity::IContextSetup* s : contexts) {
        s->resolveImports();
    }

    for (modularity::IContextSetup* s : contexts) {
        s->onPreInit(runMode);
    }

    for (modularity::IContextSetup* s : contexts) {
        s->onInit(runMode);
    }

    for (modularity::IContextSetup* s : contexts) {
        s->onAllInited(runMode);
    }

#endif

    return ctx;
}

void ConsoleApp::finish()
{
    PROFILER_PRINT;

// Wait Thread Poll
#ifndef Q_OS_WASM
    QThreadPool* globalThreadPool = QThreadPool::globalInstance();
    if (globalThreadPool) {
        LOGI() << "activeThreadCount: " << globalThreadPool->activeThreadCount();
        globalThreadPool->waitForDone();
    }
#endif

    // Deinit
    async::processMessages();

    for (modularity::IModuleSetup* m : m_modules) {
        m->onDeinit();
    }

    m_globalModule.onDeinit();

    for (modularity::IModuleSetup* m : m_modules) {
        m->onDestroy();
    }

    m_globalModule.onDestroy();

    // Delete contexts
    for (auto& c : m_contexts) {
        qDeleteAll(c.setups);
    }

    // Delete modules
    qDeleteAll(m_modules);
    m_modules.clear();

    removeIoC();
}

void ConsoleApp::applyCommandLineOptions(const CmdOptions& options, IApplication::RunMode runMode)
{
    if (options.app.loggerLevel) {
        m_globalModule.setLoggerLevel(options.app.loggerLevel.value());
    }

    if (runMode == IApplication::RunMode::AudioPluginRegistration) {
        return;
    }

    uiConfiguration()->setPhysicalDotsPerInch(options.ui.physicalDotsPerInch);

    notationConfiguration()->setTemplateModeEnabled(options.notation.templateModeEnabled);
    notationConfiguration()->setTestModeEnabled(options.notation.testModeEnabled);

    if (runMode == IApplication::RunMode::ConsoleApp) {
        project::MigrationOptions migration;
        migration.appVersion = mu::engraving::Constants::MSC_VERSION;

        //! NOTE Don't ask about migration in convert mode
        migration.isAskAgain = false;

        if (options.project.fullMigration) {
            bool isMigration = options.project.fullMigration.value();
            migration.isApplyMigration = isMigration;
            migration.isApplyEdwin = isMigration;
            migration.isApplyLeland = isMigration;
            migration.isRemapPercussion = isMigration;
        }

        //! NOTE Don't write to settings, just on current session
        for (project::MigrationType type : project::allMigrationTypes()) {
            projectConfiguration()->setMigrationOptions(type, migration, false);
        }
    }

#ifdef MUE_BUILD_IMPEXP_IMAGESEXPORT_MODULE
    imagesExportConfiguration()->setTrimMarginPixelSize(options.exportImage.trimMarginPixelSize);
    imagesExportConfiguration()->setExportPngDpiResolutionOverride(options.exportImage.pngDpiResolution);
#endif

#ifdef MUE_BUILD_IMPEXP_VIDEOEXPORT_MODULE
    videoExportConfiguration()->setResolution(options.exportVideo.resolution);
    videoExportConfiguration()->setFps(options.exportVideo.fps);
    videoExportConfiguration()->setLeadingSec(options.exportVideo.leadingSec);
    videoExportConfiguration()->setTrailingSec(options.exportVideo.trailingSec);
#endif

#ifdef MUE_BUILD_IMPEXP_MIDI_MODULE
    midiImportExportConfiguration()->setMidiImportOperationsFile(options.importMidi.operationsFile);
#endif
#ifdef MUE_BUILD_IMPEXP_MUSICXML_MODULE
    musicXmlConfiguration()->setNeedUseDefaultFontOverride(options.importMusicXml.useDefaultFont);
    musicXmlConfiguration()->setInferTextTypeOverride(options.importMusicXml.inferTextType);
#endif
#ifdef MUE_BUILD_IMPEXP_AUDIOEXPORT_MODULE
    audioExportConfiguration()->setExportMp3BitrateOverride(options.exportAudio.mp3Bitrate);
#endif
#ifdef MUE_BUILD_IMPEXP_GUITARPRO_MODULE
    guitarProConfiguration()->setLinkedTabStaffCreated(options.guitarPro.linkedTabStaffCreated);
    guitarProConfiguration()->setExperimental(options.guitarPro.experimental);
#endif
    if (options.app.revertToFactorySettings) {
        settings()->reset(options.app.revertToFactorySettings.value());
    }
}

int ConsoleApp::processConverter(const CmdOptions::ConverterTask& task)
{
    Ret ret = make_ret(Ret::Code::Ok);
    String soundProfile = task.params[CmdOptions::ParamKey::SoundProfile].toString();
    UriQuery extensionUri = UriQuery(task.params[CmdOptions::ParamKey::ExtensionUri].toString().toStdString());

    if (!soundProfile.isEmpty() && !soundProfilesRepository()->containsProfile(soundProfile)) {
        LOGE() << "Unknown sound profile: " << soundProfile;
        soundProfile.clear();
    }

    converter::OpenParams openParams;
    openParams.stylePath = task.params[CmdOptions::ParamKey::StylePath].toString();
    openParams.forceMode = task.params[CmdOptions::ParamKey::ForceMode].toBool();
    openParams.unrollRepeats = task.params[CmdOptions::ParamKey::UnrollRepeats].toBool();

    switch (task.type) {
    case ConvertType::Batch:
        ret = converter()->batchConvert(task.inputFile, openParams, soundProfile, extensionUri);
        break;
    case ConvertType::File: {
        std::string transposeOptionsJson = task.params[CmdOptions::ParamKey::ScoreTransposeOptions].toString().toStdString();
        std::optional<ConvertTarget> target = parseTarget(task.params);
        io::path_t tracksDiffPath = task.params[CmdOptions::ParamKey::TracksDiffPath].toString();
        ret = converter()->fileConvert(task.inputFile, task.outputFile, openParams, soundProfile, tracksDiffPath,
                                       extensionUri, transposeOptionsJson, target);
    } break;
    case ConvertType::ConvertScoreParts:
        ret = converter()->convertScoreParts(task.inputFile, task.outputFile, openParams);
        break;
    case ConvertType::ExportScoreMedia: {
        muse::io::path_t highlightConfigPath = task.params[CmdOptions::ParamKey::HighlightConfigPath].toString();
        ret = converter()->exportScoreMedia(task.inputFile, task.outputFile, openParams, highlightConfigPath);
    } break;
    case ConvertType::ExportScoreMeta:
        ret = converter()->exportScoreMeta(task.inputFile, task.outputFile, openParams);
        break;
    case ConvertType::ExportScoreParts:
        ret = converter()->exportScoreParts(task.inputFile, task.outputFile, openParams);
        break;
    case ConvertType::ExportScorePartsPdf:
        ret = converter()->exportScorePartsPdfs(task.inputFile, task.outputFile, openParams);
        break;
    case ConvertType::ExportScoreTranspose: {
        std::string scoreTranspose = task.params[CmdOptions::ParamKey::ScoreTransposeOptions].toString().toStdString();
        ret = converter()->exportScoreTranspose(task.inputFile, task.outputFile, scoreTranspose, openParams);
    } break;
    case ConvertType::ExportScoreElements: {
        ret = converter()->exportScoreElements(task.inputFile, task.outputFile, openParams);
    } break;
    case ConvertType::ExportScoreVideo: {
        bool withAudio = !task.params[CmdOptions::ParamKey::NoAudio].toBool();
        ret = converter()->exportScoreVideo(task.inputFile, task.outputFile, openParams, withAudio);
    } break;
    case ConvertType::SourceUpdate: {
        std::string scoreSource = task.params[CmdOptions::ParamKey::ScoreSource].toString().toStdString();
        ret = converter()->updateSource(task.inputFile, scoreSource, openParams.forceMode);
    } break;
    case ConvertType::PipeServer:
        break; // handled before processConverter() is called
    }

    if (!ret) {
        LOGE() << "failed convert, error: " << ret.toString();
    }

    return ret.code();
}

void ConsoleApp::processPipeServer()
{
    auto buffer = std::make_shared<QByteArray>();

    fputs("{\"status\":\"ready\"}\n", stdout);
    fflush(stdout);

    // Use a background thread to block-read stdin. QSocketNotifier works on Linux
    // but not on Windows pipes, so a thread is the portable solution.
    auto processChunk = [this, buffer](const QByteArray& chunk) {
        buffer->append(chunk);

        int idx;
        while ((idx = buffer->indexOf('\n')) >= 0) {
            QByteArray line = buffer->left(idx).trimmed();
            buffer->remove(0, idx + 1);
            if (line.isEmpty()) continue;

            QJsonParseError parseErr;
            QJsonDocument doc = QJsonDocument::fromJson(line, &parseErr);
            if (doc.isNull() || !doc.isObject()) {
                fputs("{\"status\":\"error\",\"message\":\"Invalid JSON\"}\n", stdout);
                fflush(stdout);
                continue;
            }

            QJsonObject req = doc.object();
            QString inFile  = req[u"in"].toString();
            QString outFile = req[u"out"].toString();

            if (inFile.isEmpty() || outFile.isEmpty()) {
                fputs("{\"status\":\"error\",\"message\":\"Missing 'in' or 'out' field\"}\n", stdout);
                fflush(stdout);
                continue;
            }

#ifdef MUE_BUILD_IMPEXP_IMAGESEXPORT_MODULE
            if (req.contains(u"dpi"))
                imagesExportConfiguration()->setExportPngDpiResolutionOverride(
                    static_cast<float>(req[u"dpi"].toDouble(96.0)));
            if (req.contains(u"trim"))
                imagesExportConfiguration()->setTrimMarginPixelSize(req[u"trim"].toInt(0));
#endif

            converter::OpenParams openParams;
            if (req.contains(u"style_path"))
                openParams.stylePath = muse::io::path_t(req[u"style_path"].toString().toStdString());

            Ret ret = converter()->fileConvert(
                muse::io::path_t(inFile.toStdString()),
                muse::io::path_t(outFile.toStdString()),
                openParams, {}, {}, {}, {}, {});

            if (ret) {
                fputs("{\"status\":\"ok\"}\n", stdout);
            } else {
                QJsonObject resp;
                resp[u"status"]  = QString("error");
                resp[u"message"] = QString::fromStdString(ret.toString());
                QByteArray respJson = QJsonDocument(resp).toJson(QJsonDocument::Compact) + "\n";
                fwrite(respJson.constData(), 1, static_cast<size_t>(respJson.size()), stdout);
            }
            fflush(stdout);
        }
    };

    QThread* readerThread = QThread::create([processChunk]() {
        char tmp[8192];
        while (true) {
            ssize_t n = posix_read(STDIN_FILENO, tmp, sizeof(tmp) - 1);
            if (n <= 0) {
                QMetaObject::invokeMethod(qApp, []() { qApp->exit(0); }, Qt::QueuedConnection);
                return;
            }
            QByteArray chunk(tmp, static_cast<int>(n));
            QMetaObject::invokeMethod(qApp, [processChunk, chunk]() { processChunk(chunk); },
                                      Qt::QueuedConnection);
        }
    });
    readerThread->start();
}

int ConsoleApp::processDiagnostic(const CmdOptions::Diagnostic& task)
{
    if (!diagnosticDrawProvider()) {
        return make_ret(Ret::Code::NotSupported);
    }

    Ret ret = make_ret(Ret::Code::Ok);

    if (task.input.isEmpty()) {
        return make_ret(Ret::Code::UnknownError);
    }

    io::paths_t input;
    for (const QString& p : task.input) {
        input.push_back(p);
    }

    muse::io::path_t output = task.output;

    if (output.empty()) {
        output = "./";
    }

    switch (task.type) {
    case DiagnosticType::GenDrawData:
        ret = diagnosticDrawProvider()->generateDrawData(input.front(), output);
        break;
    case DiagnosticType::ComDrawData: {
        IF_ASSERT_FAILED(input.size() == 2) {
            return make_ret(Ret::Code::UnknownError);
        }
        ret = diagnosticDrawProvider()->compareDrawData(input.at(0), input.at(1), output);
    } break;
    case DiagnosticType::DrawDataToPng:
        ret = diagnosticDrawProvider()->drawDataToPng(input.front(), output);
        break;
    case DiagnosticType::DrawDiffToPng: {
        muse::io::path_t diffPath = input.at(0);
        muse::io::path_t refPath;
        if (input.size() > 1) {
            refPath = input.at(1);
        }
        ret = diagnosticDrawProvider()->drawDiffToPng(diffPath, refPath, output);
    } break;
    default:
        break;
    }

    if (!ret) {
        LOGE() << "diagnostic ret: " << ret.toString();
    }

    return ret.code();
}

int ConsoleApp::processAudioPluginRegistration(const CmdOptions::AudioPluginRegistration& task)
{
    Ret ret = make_ret(Ret::Code::Ok);

    if (task.failedPlugin) {
        LOGI() << "Register incompatible plugin: " << task.pluginPath << ", err code: " << task.failCode;
        ret = registerAudioPluginsScenario()->registerFailedPlugin(task.pluginPath, task.failCode);
    } else {
        LOGI() << "Register plugin: " << task.pluginPath;
        ret = registerAudioPluginsScenario()->registerPlugin(task.pluginPath);
    }

    if (!ret) {
        LOGE() << ret.toString();
    }

    return ret.code();
}

void ConsoleApp::processAutobot(const CmdOptions::Autobot& task)
{
    using namespace muse::autobot;
    muse::async::Channel<StepInfo, Ret> stepCh = autobot()->stepStatusChanged();
    stepCh.onReceive(nullptr, [](const StepInfo& step, const Ret& ret){
        if (!ret) {
            LOGE() << "failed step: " << step.name << ", ret: " << ret.toString();
            qApp->exit(ret.code());
        } else {
            LOGI() << "success step: " << step.name << ", ret: " << ret.toString();
        }
    });

    muse::async::Channel<muse::io::path_t, IAutobot::Status> statusCh = autobot()->statusChanged();
    statusCh.onReceive(nullptr, [](const muse::io::path_t& path, IAutobot::Status st){
        if (st == IAutobot::Status::Finished) {
            LOGI() << "success finished, path: " << path;
            qApp->exit(0);
        }
    });

    IAutobot::Options opt;
    opt.context = task.testCaseContextNameOrFile;
    opt.contextVal = task.testCaseContextValue.toStdString();
    opt.func = task.testCaseFunc.toStdString();
    opt.funcArgs = task.testCaseFuncArgs.toStdString();

    autobot()->execScript(task.testCaseNameOrFile, opt);
}
