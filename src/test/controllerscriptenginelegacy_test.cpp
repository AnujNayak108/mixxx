#include "controllers/scripting/legacy/controllerscriptenginelegacy.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <QByteArrayView>
#include <QMetaEnum>
#include <QScopedPointer>
#include <QTemporaryFile>
#include <QTest>
#include <QThread>
#include <QtDebug>
#include <bit>
#include <memory>

#include "control/controlobject.h"
#include "control/controlpotmeter.h"
#include "controllers/scripting/legacy/controllerscriptinterfacelegacy.h"
#ifdef MIXXX_USE_QML
#include <QQuickItem>

#include "controllers/controllerenginethreadcontrol.h"
#include "controllers/rendering/controllerrenderingengine.h"
#endif
#include "controllers/softtakeover.h"
#include "helpers/log_test.h"
#include "preferences/usersettings.h"
#ifdef MIXXX_USE_QML
#include "qml/qmlmixxxcontrollerscreen.h"
#endif
#include "control/controlindicatortimer.h"
#include "database/mixxxdb.h"
#include "effects/effectsmanager.h"
#include "engine/channelhandle.h"
#include "engine/channels/enginedeck.h"
#include "engine/enginebuffer.h"
#include "engine/enginemixer.h"
#include "library/coverartcache.h"
#include "library/library.h"
#include "library/trackcollectionmanager.h"
#include "mixer/deck.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "recording/recordingmanager.h"
#include "soundio/soundmanager.h"
#include "sources/soundsourceproxy.h"
#include "test/mixxxdbtest.h"
#include "test/mixxxtest.h"
#include "test/soundsourceproviderregistration.h"
#include "track/track.h"
#include "util/color/colorpalette.h"
#include "util/time.h"

using ::testing::_;
using namespace std::chrono_literals;

typedef std::unique_ptr<QTemporaryFile> ScopedTemporaryFile;

const RuntimeLoggingCategory logger(QString("test").toLocal8Bit());

class ControllerScriptEngineLegacyTest : public ControllerScriptEngineLegacy,
                                         public MixxxDbTest,
                                         SoundSourceProviderRegistration {
  protected:
    ControllerScriptEngineLegacyTest()
            : ControllerScriptEngineLegacy(nullptr, logger) {
    }
    static ScopedTemporaryFile makeTemporaryFile(const QString& contents) {
        QByteArray contentsBa = contents.toLocal8Bit();
        ScopedTemporaryFile pFile = std::make_unique<QTemporaryFile>();
        pFile->open();
        pFile->write(contentsBa);
        pFile->close();
        return pFile;
    }

    void SetUp() override {
        mixxx::Time::setTestMode(true);
        mixxx::Time::addTestTime(10ms);
        QThread::currentThread()->setObjectName("Main");
        initialize();

        // This setup mirrors coreservices -- it would be nice if we could use coreservices instead
        // but it does a lot of local disk / settings setup.
        auto pChannelHandleFactory = std::make_shared<ChannelHandleFactory>();
        m_pEffectsManager = std::make_shared<EffectsManager>(config(), pChannelHandleFactory);
        m_pEngine = std::make_shared<EngineMixer>(
                config(),
                "[Master]",
                m_pEffectsManager.get(),
                pChannelHandleFactory,
                true);
        m_pSoundManager = std::make_shared<SoundManager>(config(), m_pEngine.get());
        m_pControlIndicatorTimer = std::make_shared<mixxx::ControlIndicatorTimer>(nullptr);
        m_pEngine->registerNonEngineChannelSoundIO(gsl::make_not_null(m_pSoundManager.get()));

        CoverArtCache::createInstance();

        m_pPlayerManager = std::make_shared<PlayerManager>(config(),
                m_pSoundManager.get(),
                m_pEffectsManager.get(),
                m_pEngine.get());

        m_pPlayerManager->addConfiguredDecks();
        m_pPlayerManager->addSampler();
        PlayerInfo::create();
        m_pEffectsManager->setup();

        const auto dbConnection = mixxx::DbConnectionPooled(dbConnectionPooler());
        if (!MixxxDb::initDatabaseSchema(dbConnection)) {
            exit(1);
        }

        m_pTrackCollectionManager = std::make_shared<TrackCollectionManager>(
                nullptr,
                config(),
                dbConnectionPooler(),
                [](Track* pTrack) { delete pTrack; });

        m_pRecordingManager = std::make_shared<RecordingManager>(config(), m_pEngine.get());
        m_pLibrary = std::make_shared<Library>(
                nullptr,
                config(),
                dbConnectionPooler(),
                m_pTrackCollectionManager.get(),
                m_pPlayerManager.get(),
                m_pRecordingManager.get());

        m_pPlayerManager->bindToLibrary(m_pLibrary.get());
        ControllerScriptEngineBase::registerPlayerManager(m_pPlayerManager);
        ControllerScriptEngineBase::registerTrackCollectionManager(m_pTrackCollectionManager);
    }

    void loadTrackSync(const QString& trackLocation) {
        TrackPointer pTrack1 = m_pTrackCollectionManager->getOrAddTrack(
                TrackRef::fromFilePath(getTestDir().filePath(trackLocation)));
        auto* deck = m_pPlayerManager->getDeck(1);
        deck->slotLoadTrack(pTrack1,
#ifdef __STEM__
                mixxx::StemChannelSelection(),
#endif
                false);
        m_pEngine->process(1024);
        while (!deck->getEngineDeck()->getEngineBuffer()->isTrackLoaded()) {
            QTest::qSleep(100);
        }
        processEvents();
    }

    void TearDown() override {
        mixxx::Time::setTestMode(false);
#ifdef MIXXX_USE_QML
        m_rootItems.clear();
#endif
        CoverArtCache::destroy();
        ControllerScriptEngineBase::registerPlayerManager(nullptr);
        ControllerScriptEngineBase::registerTrackCollectionManager(nullptr);
    }

    ~ControllerScriptEngineLegacyTest() {
        // Reset in the correct order to avoid singleton destruction issues
        m_pSoundManager.reset();
        m_pPlayerManager.reset();
        PlayerInfo::destroy();
        m_pLibrary.reset();
        m_pRecordingManager.reset();
        m_pEngine.reset();
        m_pEffectsManager.reset();
        m_pTrackCollectionManager.reset();
        m_pControlIndicatorTimer.reset();
    }

    bool evaluateScriptFile(const QFileInfo& scriptFile) {
        return ControllerScriptEngineLegacy::evaluateScriptFile(scriptFile);
    }

    QJSValue evaluate(const QString& code) {
        return jsEngine()->evaluate(code);
    }

    bool evaluateAndAssert(const QString& code) {
        return !evaluate(code).isError();
    }

    void processEvents() {
        // QCoreApplication::processEvents() only processes events that were
        // queued when the method was called. Hence, all subsequent events that
        // are emitted while processing those queued events will not be
        // processed and are enqueued for the next event processing cycle.
        // Calling processEvents() twice ensures that at least all queued and
        // the next round of emitted events are processed.
        application()->processEvents();
        application()->processEvents();
    }

#ifdef MIXXX_USE_QML
    QHash<QString, std::shared_ptr<ControllerRenderingEngine>>& renderingScreens() {
        return m_renderingScreens;
    }

    std::unordered_map<QString,
            std::unique_ptr<mixxx::qml::QmlMixxxControllerScreen>>&
    rootItems() {
        return m_rootItems;
    }

    void testHandleScreen(
            const LegacyControllerMapping::ScreenInfo& screeninfo,
            const QImage& frame,
            const QDateTime& timestamp) {
        handleScreenFrame(screeninfo, frame, timestamp);
    }
#endif

    std::shared_ptr<EffectsManager> m_pEffectsManager;
    std::shared_ptr<EngineMixer> m_pEngine;
    std::shared_ptr<SoundManager> m_pSoundManager;
    std::shared_ptr<mixxx::ControlIndicatorTimer> m_pControlIndicatorTimer;
    std::shared_ptr<PlayerManager> m_pPlayerManager;
    std::shared_ptr<RecordingManager> m_pRecordingManager;
    std::shared_ptr<Library> m_pLibrary;
    std::shared_ptr<TrackCollectionManager> m_pTrackCollectionManager;
};

class ControllerScriptEngineLegacyTimerTest : public ControllerScriptEngineLegacyTest {
  protected:
    std::unique_ptr<ControlPotmeter> co;
    std::unique_ptr<ControlPotmeter> coTimerId;

    void SetUp() override {
        ControllerScriptEngineLegacyTest::SetUp();
        co = std::make_unique<ControlPotmeter>(ConfigKey("[Test]", "co"), -10.0, 10.0);
        co->setParameter(0.0);
        coTimerId = std::make_unique<ControlPotmeter>(
                ConfigKey("[Test]", "coTimerId"), -10.0, 50.0);
        coTimerId->setParameter(0.0);
        EXPECT_TRUE(evaluateAndAssert("engine.setValue('[Test]', 'co', 0.0);"));
        EXPECT_DOUBLE_EQ(0.0, co->get());
    }
};
TEST_F(ControllerScriptEngineLegacyTest, commonScriptHasNoErrors) {
    QFileInfo commonScript(config()->getResourcePath() +
            QStringLiteral("/controllers/common-controller-scripts.js"));
    EXPECT_TRUE(evaluateScriptFile(commonScript));
}

TEST_F(ControllerScriptEngineLegacyTest, setValue) {
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    EXPECT_TRUE(evaluateAndAssert("engine.setValue('[Test]', 'co', 1.0);"));
    EXPECT_DOUBLE_EQ(1.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTest, getValue_InvalidKey) {
    EXPECT_TRUE(evaluateAndAssert("engine.getValue('', '');"));
    EXPECT_TRUE(evaluateAndAssert("engine.getValue('', 'invalid');"));
    EXPECT_TRUE(evaluateAndAssert("engine.getValue('[Invalid]', '');"));
}

TEST_F(ControllerScriptEngineLegacyTest, setValue_InvalidControl) {
    EXPECT_TRUE(evaluateAndAssert("engine.setValue('[Nothing]', 'nothing', 1.0);"));
}

TEST_F(ControllerScriptEngineLegacyTest, getValue_InvalidControl) {
    EXPECT_TRUE(evaluateAndAssert("engine.getValue('[Nothing]', 'nothing');"));
}

TEST_F(ControllerScriptEngineLegacyTest, setValue_IgnoresNaN) {
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    co->set(10.0);
    EXPECT_TRUE(evaluateAndAssert("engine.setValue('[Test]', 'co', NaN);"));
    EXPECT_DOUBLE_EQ(10.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTest, getSetValue) {
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    EXPECT_TRUE(
            evaluateAndAssert("engine.setValue('[Test]', 'co', "
                              "engine.getValue('[Test]', 'co') + 1);"));
    EXPECT_DOUBLE_EQ(1.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTest, setParameter) {
    auto co = std::make_unique<ControlPotmeter>(ConfigKey("[Test]", "co"),
            -10.0,
            10.0);
    EXPECT_TRUE(evaluateAndAssert("engine.setParameter('[Test]', 'co', 1.0);"));
    EXPECT_DOUBLE_EQ(10.0, co->get());
    EXPECT_TRUE(evaluateAndAssert("engine.setParameter('[Test]', 'co', 0.0);"));
    EXPECT_DOUBLE_EQ(-10.0, co->get());
    EXPECT_TRUE(evaluateAndAssert("engine.setParameter('[Test]', 'co', 0.5);"));
    EXPECT_DOUBLE_EQ(0.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTest, setParameter_OutOfRange) {
    auto co = std::make_unique<ControlPotmeter>(ConfigKey("[Test]", "co"),
            -10.0,
            10.0);
    EXPECT_TRUE(evaluateAndAssert("engine.setParameter('[Test]', 'co', 1000);"));
    EXPECT_DOUBLE_EQ(10.0, co->get());
    EXPECT_TRUE(evaluateAndAssert("engine.setParameter('[Test]', 'co', -1000);"));
    EXPECT_DOUBLE_EQ(-10.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTest, setParameter_NaN) {
    // Test that NaNs are ignored.
    auto co = std::make_unique<ControlPotmeter>(ConfigKey("[Test]", "co"),
            -10.0,
            10.0);
    EXPECT_TRUE(evaluateAndAssert("engine.setParameter('[Test]', 'co', NaN);"));
    EXPECT_DOUBLE_EQ(0.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTest, getSetParameter) {
    auto co = std::make_unique<ControlPotmeter>(ConfigKey("[Test]", "co"),
            -10.0,
            10.0);
    EXPECT_TRUE(evaluateAndAssert(
            "engine.setParameter('[Test]', 'co', "
            "  engine.getParameter('[Test]', 'co') + 0.1);"));
    EXPECT_DOUBLE_EQ(2.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTest, softTakeover_setValue) {
    auto co = std::make_unique<ControlPotmeter>(ConfigKey("[Test]", "co"),
            -10.0,
            10.0);
    co->setParameter(0.0);
    EXPECT_TRUE(evaluateAndAssert(
            "engine.softTakeover('[Test]', 'co', true);"
            "engine.setValue('[Test]', 'co', 0.0);"));
    // The first set after enabling is always ignored.
    EXPECT_DOUBLE_EQ(-10.0, co->get());

    // Change the control internally (putting it out of sync with the
    // ControllerEngine).
    co->setParameter(0.5);

    // Time elapsed is not greater than the threshold, so we do not ignore this
    // set.
    EXPECT_TRUE(evaluateAndAssert("engine.setValue('[Test]', 'co', -10.0);"));
    EXPECT_DOUBLE_EQ(-10.0, co->get());

    // Advance time to 2x the threshold.
    SoftTakeover::TestAccess::advanceTimePastThreshold();

    // Change the control internally (putting it out of sync with the
    // ControllerEngine).
    co->setParameter(0.5);

    // Ignore the change since it occurred after the threshold and is too large.
    EXPECT_TRUE(evaluateAndAssert("engine.setValue('[Test]', 'co', -10.0);"));
    EXPECT_DOUBLE_EQ(0.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTest, softTakeover_setParameter) {
    auto co = std::make_unique<ControlPotmeter>(ConfigKey("[Test]", "co"),
            -10.0,
            10.0);
    co->setParameter(0.0);
    EXPECT_TRUE(evaluateAndAssert(
            "engine.softTakeover('[Test]', 'co', true);"
            "engine.setParameter('[Test]', 'co', 1.0);"));
    // The first set after enabling is always ignored.
    EXPECT_DOUBLE_EQ(-10.0, co->get());

    // Change the control internally (putting it out of sync with the
    // ControllerEngine).
    co->setParameter(0.5);

    // Time elapsed is not greater than the threshold, so we do not ignore this
    // set.
    EXPECT_TRUE(evaluateAndAssert("engine.setParameter('[Test]', 'co', 0.0);"));
    EXPECT_DOUBLE_EQ(-10.0, co->get());

    SoftTakeover::TestAccess::advanceTimePastThreshold();

    // Change the control internally (putting it out of sync with the
    // ControllerEngine).
    co->setParameter(0.5);

    // Ignore the change since it occurred after the threshold and is too large.
    EXPECT_TRUE(evaluateAndAssert("engine.setParameter('[Test]', 'co', 0.0);"));
    EXPECT_DOUBLE_EQ(0.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTest, softTakeover_ignoreNextValue) {
    auto co = std::make_unique<ControlPotmeter>(ConfigKey("[Test]", "co"),
            -10.0,
            10.0);
    co->setParameter(0.0);
    EXPECT_TRUE(evaluateAndAssert(
            "engine.softTakeover('[Test]', 'co', true);"
            "engine.setParameter('[Test]', 'co', 1.0);"));
    // The first set after enabling is always ignored.
    EXPECT_DOUBLE_EQ(-10.0, co->get());

    // Change the control internally (putting it out of sync with the
    // ControllerEngine).
    co->setParameter(0.5);

    EXPECT_TRUE(evaluateAndAssert("engine.softTakeoverIgnoreNextValue('[Test]', 'co');"));

    // We would normally allow this set since it is below the time threshold,
    // but we are ignoring the next value.
    EXPECT_TRUE(evaluateAndAssert("engine.setParameter('[Test]', 'co', 0.0);"));
    EXPECT_DOUBLE_EQ(0.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTest, reset) {
    // Test that NaNs are ignored.
    auto co = std::make_unique<ControlPotmeter>(ConfigKey("[Test]", "co"),
            -10.0,
            10.0);
    co->setParameter(1.0);
    EXPECT_TRUE(evaluateAndAssert("engine.reset('[Test]', 'co');"));
    EXPECT_DOUBLE_EQ(0.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTest, log) {
    EXPECT_TRUE(evaluateAndAssert("engine.log('Test that logging works.');"));
}

TEST_F(ControllerScriptEngineLegacyTest, trigger) {
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    auto pass = std::make_unique<ControlObject>(ConfigKey("[Test]", "passed"));

    EXPECT_TRUE(evaluateAndAssert(
            "var reaction = function(value) { "
            "  let pass = engine.getValue('[Test]', 'passed');"
            "  engine.setValue('[Test]', 'passed', pass + 1.0); };"
            "var connection = engine.connectControl('[Test]', 'co', reaction);"
            "engine.trigger('[Test]', 'co');"));
    // ControlObjectScript connections are processed via QueuedConnection. Use
    // processEvents() to cause Qt to deliver them.
    processEvents();
    // The counter should have been incremented exactly once.
    EXPECT_DOUBLE_EQ(1.0, pass->get());
}

// ControllerEngine::connectControl has a lot of quirky, inconsistent legacy behaviors
// depending on how it is invoked, so we need a lot of tests to make sure old scripts
// do not break.

TEST_F(ControllerScriptEngineLegacyTest, connectControl_ByString) {
    // Test that connecting and disconnecting by function name works.
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    auto pass = std::make_unique<ControlObject>(ConfigKey("[Test]", "passed"));

    EXPECT_TRUE(evaluateAndAssert(
            "var reaction = function(value) { "
            "  let pass = engine.getValue('[Test]', 'passed');"
            "  engine.setValue('[Test]', 'passed', pass + 1.0); };"
            "engine.connectControl('[Test]', 'co', 'reaction');"
            "engine.trigger('[Test]', 'co');"
            "function disconnect() { "
            "  engine.connectControl('[Test]', 'co', 'reaction', 1);"
            "  engine.trigger('[Test]', 'co'); }"));
    // ControlObjectScript connections are processed via QueuedConnection. Use
    // processEvents() to cause Qt to deliver them.
    processEvents();
    EXPECT_TRUE(evaluateAndAssert("disconnect();"));
    processEvents();
    // The counter should have been incremented exactly once.
    EXPECT_DOUBLE_EQ(1.0, pass->get());
}

TEST_F(ControllerScriptEngineLegacyTest, connectControl_ByStringForbidDuplicateConnections) {
    // Test that connecting a control to a callback specified by a string
    // does not make duplicate connections. This behavior is inconsistent
    // with the behavior when specifying a callback as a function, but
    // this is how it has been done, so keep the behavior to ensure old scripts
    // do not break.
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    auto pass = std::make_unique<ControlObject>(ConfigKey("[Test]", "passed"));

    EXPECT_TRUE(evaluateAndAssert(
            "var reaction = function(value) { "
            "  let pass = engine.getValue('[Test]', 'passed');"
            "  engine.setValue('[Test]', 'passed', pass + 1.0); };"
            "engine.connectControl('[Test]', 'co', 'reaction');"
            "engine.connectControl('[Test]', 'co', 'reaction');"
            "engine.trigger('[Test]', 'co');"));
    // ControlObjectScript connections are processed via QueuedConnection. Use
    // processEvents() to cause Qt to deliver them.
    processEvents();
    // The counter should have been incremented exactly once.
    EXPECT_DOUBLE_EQ(1.0, pass->get());
}

TEST_F(ControllerScriptEngineLegacyTest,
        connectControl_ByStringRedundantConnectionObjectsAreNotIndependent) {
    // Test that multiple connections are not allowed when passing
    // the callback to engine.connectControl as a function name string.
    // This is weird and inconsistent, but it is how it has been done,
    // so keep this behavior to make sure old scripts do not break.
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    auto counter = std::make_unique<ControlObject>(ConfigKey("[Test]", "counter"));

    QString script(
            "var incrementCounterCO = function () {"
            "  let counter = engine.getValue('[Test]', 'counter');"
            "  engine.setValue('[Test]', 'counter', counter + 1);"
            "};"
            "var connection1 = engine.connectControl('[Test]', 'co', 'incrementCounterCO');"
            // Make a second connection with the same ControlObject
            // to check that disconnecting one does not disconnect both.
            "var connection2 = engine.connectControl('[Test]', 'co', 'incrementCounterCO');"
            "function changeTestCoValue() {"
            "  let testCoValue = engine.getValue('[Test]', 'co');"
            "  engine.setValue('[Test]', 'co', testCoValue + 1);"
            "};"
            "function disconnectConnection2() {"
            "  connection2.disconnect();"
            "};");

    evaluateAndAssert(script);
    EXPECT_TRUE(evaluateAndAssert(script));
    evaluateAndAssert("changeTestCoValue()");
    // ControlObjectScript connections are processed via QueuedConnection. Use
    // processEvents() to cause Qt to deliver them.
    processEvents();
    EXPECT_EQ(1.0, counter->get());

    evaluateAndAssert("disconnectConnection2()");
    // The connection objects should refer to the same connection,
    // so disconnecting one should disconnect both.
    evaluateAndAssert("changeTestCoValue()");
    processEvents();
    EXPECT_EQ(1.0, counter->get());
}

TEST_F(ControllerScriptEngineLegacyTest, connectControl_ByFunction) {
    // Test that connecting and disconnecting with a function value works.
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    auto pass = std::make_unique<ControlObject>(ConfigKey("[Test]", "passed"));

    EXPECT_TRUE(evaluateAndAssert(
            "var reaction = function(value) { "
            "  let pass = engine.getValue('[Test]', 'passed');"
            "  engine.setValue('[Test]', 'passed', pass + 1.0); };"
            "var connection = engine.connectControl('[Test]', 'co', reaction);"
            "connection.trigger();"));
    // ControlObjectScript connections are processed via QueuedConnection. Use
    // processEvents() to cause Qt to deliver them.
    processEvents();
    // The counter should have been incremented exactly once.
    EXPECT_DOUBLE_EQ(1.0, pass->get());
}

TEST_F(ControllerScriptEngineLegacyTest, connectControl_ByFunctionAllowDuplicateConnections) {
    // Test that duplicate connections are allowed when passing callbacks as functions.
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    auto pass = std::make_unique<ControlObject>(ConfigKey("[Test]", "passed"));

    EXPECT_TRUE(evaluateAndAssert(
            "var reaction = function(value) { "
            "  let pass = engine.getValue('[Test]', 'passed');"
            "  engine.setValue('[Test]', 'passed', pass + 1.0); };"
            "engine.connectControl('[Test]', 'co', reaction);"
            "engine.connectControl('[Test]', 'co', reaction);"
            // engine.trigger() has no way to know which connection to a ControlObject
            // to trigger, so it should trigger all of them.
            "engine.trigger('[Test]', 'co');"));
    // ControlObjectScript connections are processed via QueuedConnection. Use
    // processEvents() to cause Qt to deliver them.
    processEvents();
    // The counter should have been incremented exactly twice.
    EXPECT_DOUBLE_EQ(2.0, pass->get());
}

TEST_F(ControllerScriptEngineLegacyTest, connectControl_toDisconnectRemovesAllConnections) {
    // Test that every connection to a ControlObject is disconnected
    // by calling engine.connectControl(..., true). Individual connections
    // can only be disconnected by storing the connection object returned by
    // engine.connectControl and calling that object's 'disconnect' method.
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    auto pass = std::make_unique<ControlObject>(ConfigKey("[Test]", "passed"));

    EXPECT_TRUE(evaluateAndAssert(
            "var reaction = function(value) { "
            "  let pass = engine.getValue('[Test]', 'passed');"
            "  engine.setValue('[Test]', 'passed', pass + 1.0); };"
            "engine.connectControl('[Test]', 'co', reaction);"
            "engine.connectControl('[Test]', 'co', reaction);"
            "engine.trigger('[Test]', 'co');"
            "function disconnect() { "
            "  engine.connectControl('[Test]', 'co', reaction, 1);"
            "  engine.trigger('[Test]', 'co'); }"));
    // ControlObjectScript connections are processed via QueuedConnection. Use
    // processEvents() to cause Qt to deliver them.
    processEvents();
    EXPECT_TRUE(evaluateAndAssert("disconnect()"));
    processEvents();
    // The counter should have been incremented exactly twice.
    EXPECT_DOUBLE_EQ(2.0, pass->get());
}

TEST_F(ControllerScriptEngineLegacyTest, connectControl_ByLambda) {
    // Test that connecting with an anonymous function works.
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    auto pass = std::make_unique<ControlObject>(ConfigKey("[Test]", "passed"));

    EXPECT_TRUE(evaluateAndAssert(
            "var connection = engine.connectControl('[Test]', 'co', function(value) { "
            "  let pass = engine.getValue('[Test]', 'passed');"
            "  engine.setValue('[Test]', 'passed', pass + 1.0); });"
            "connection.trigger();"
            "function disconnect() { "
            "  connection.disconnect();"
            "  engine.trigger('[Test]', 'co'); }"));
    // ControlObjectScript connections are processed via QueuedConnection. Use
    // processEvents() to cause Qt to deliver them.
    processEvents();
    EXPECT_TRUE(evaluateAndAssert("disconnect()"));
    processEvents();
    // The counter should have been incremented exactly once.
    EXPECT_DOUBLE_EQ(1.0, pass->get());
}

TEST_F(ControllerScriptEngineLegacyTest, connectionObject_Disconnect) {
    // Test that disconnecting using the 'disconnect' method on the connection
    // object returned from connectControl works.
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    auto pass = std::make_unique<ControlObject>(ConfigKey("[Test]", "passed"));

    EXPECT_TRUE(evaluateAndAssert(
            "var reaction = function(value) { "
            "  let pass = engine.getValue('[Test]', 'passed');"
            "  engine.setValue('[Test]', 'passed', pass + 1.0); };"
            "var connection = engine.makeConnection('[Test]', 'co', reaction);"
            "connection.trigger();"
            "function disconnect() { "
            "  connection.disconnect();"
            "  engine.trigger('[Test]', 'co'); }"));
    // ControlObjectScript connections are processed via QueuedConnection. Use
    // processEvents() to cause Qt to deliver them.
    processEvents();
    EXPECT_TRUE(evaluateAndAssert("disconnect()"));
    processEvents();
    // The counter should have been incremented exactly once.
    EXPECT_DOUBLE_EQ(1.0, pass->get());
}

TEST_F(ControllerScriptEngineLegacyTest, connectionObject_reflectDisconnect) {
    // Test that checks if disconnecting yields the appropriate feedback
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    auto pass = std::make_unique<ControlObject>(ConfigKey("[Test]", "passed"));

    EXPECT_TRUE(evaluateAndAssert(
            "var reaction = function(success) { "
            "  if (success) {"
            "    let pass = engine.getValue('[Test]', 'passed');"
            "    engine.setValue('[Test]', 'passed', pass + 1.0); "
            "  }"
            "};"
            "let dummy_callback = function(value) {};"
            "let connection = engine.makeConnection('[Test]', 'co', dummy_callback);"
            "reaction(connection);"
            "reaction(connection.isConnected);"
            "let successful_disconnect = connection.disconnect();"
            "reaction(successful_disconnect);"
            "reaction(!connection.isConnected);"));
    processEvents();
    EXPECT_DOUBLE_EQ(4.0, pass->get());
}

TEST_F(ControllerScriptEngineLegacyTest, connectionObject_DisconnectByPassingToConnectControl) {
    // Test that passing a connection object back to engine.connectControl
    // removes the connection
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    auto pass = std::make_unique<ControlObject>(ConfigKey("[Test]", "passed"));
    // The connections should be removed from the ControlObject which they were
    // actually connected to, regardless of the group and item arguments passed
    // to engine.connectControl() to remove the connection. All that should matter
    // is that a valid ControlObject is specified.
    auto dummy = std::make_unique<ControlObject>(ConfigKey("[Test]", "dummy"));

    EXPECT_TRUE(evaluateAndAssert(
            "var reaction = function(value) { "
            "  let pass = engine.getValue('[Test]', 'passed');"
            "  engine.setValue('[Test]', 'passed', pass + 1.0); };"
            "var connection1 = engine.connectControl('[Test]', 'co', reaction);"
            "var connection2 = engine.connectControl('[Test]', 'co', reaction);"
            "function disconnectConnection1() { "
            "  engine.connectControl('[Test]',"
            "                        'dummy',"
            "                        connection1);"
            "  engine.trigger('[Test]', 'co'); }"
            // Whether a 4th argument is passed to engine.connectControl does not matter.
            "function disconnectConnection2() { "
            "  engine.connectControl('[Test]',"
            "                        'dummy',"
            "                        connection2, true);"
            "  engine.trigger('[Test]', 'co'); }"));
    // ControlObjectScript connections are processed via QueuedConnection. Use
    // processEvents() to cause Qt to deliver them.
    processEvents();
    EXPECT_TRUE(evaluateAndAssert("disconnectConnection1()"));
    processEvents();
    // The counter should have been incremented once by connection2.
    EXPECT_DOUBLE_EQ(1.0, pass->get());
    EXPECT_TRUE(evaluateAndAssert("disconnectConnection2()"));
    processEvents();
    // The counter should not have changed.
    EXPECT_DOUBLE_EQ(1.0, pass->get());
}

TEST_F(ControllerScriptEngineLegacyTest, connectionObject_MakesIndependentConnection) {
    // Test that multiple connections can be made to the same CO with
    // the same callback function and that calling their 'disconnect' method
    // only disconnects the callback for that object.
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    auto counter = std::make_unique<ControlObject>(ConfigKey("[Test]", "counter"));

    EXPECT_TRUE(evaluateAndAssert(
            "var incrementCounterCO = function () {"
            "  let counter = engine.getValue('[Test]', 'counter');"
            "  engine.setValue('[Test]', 'counter', counter + 1);"
            "};"
            "var connection1 = engine.makeConnection('[Test]', 'co', incrementCounterCO);"
            // Make a second connection with the same ControlObject
            // to check that disconnecting one does not disconnect both.
            "var connection2 = engine.makeConnection('[Test]', 'co', incrementCounterCO);"
            "function changeTestCoValue() {"
            "  let testCoValue = engine.getValue('[Test]', 'co');"
            "  engine.setValue('[Test]', 'co', testCoValue + 1);"
            "}"
            "function disconnectConnection1() {"
            "  connection1.disconnect();"
            "}"));
    EXPECT_TRUE(evaluateAndAssert("changeTestCoValue()"));
    // ControlObjectScript connections are processed via QueuedConnection. Use
    // processEvents() to cause Qt to deliver them.
    processEvents();
    EXPECT_EQ(2.0, counter->get());

    EXPECT_TRUE(evaluateAndAssert("disconnectConnection1()"));
    // Only the callback for connection1 should have disconnected;
    // the callback for connection2 should still be connected, so
    // changing the CO they were both connected to should
    // increment the counter once.
    EXPECT_TRUE(evaluateAndAssert("changeTestCoValue()"));
    processEvents();
    EXPECT_EQ(3.0, counter->get());
}

TEST_F(ControllerScriptEngineLegacyTest, connectionObject_trigger) {
    // Test that triggering using the 'trigger' method on the connection
    // object returned from connectControl works.
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    auto counter = std::make_unique<ControlObject>(ConfigKey("[Test]", "counter"));

    EXPECT_TRUE(evaluateAndAssert(
            "var incrementCounterCO = function () {"
            "  let counter = engine.getValue('[Test]', 'counter');"
            "  engine.setValue('[Test]', 'counter', counter + 1);"
            "};"
            "var connection1 = engine.makeConnection('[Test]', 'co', incrementCounterCO);"
            // Make a second connection with the same ControlObject
            // to check that triggering a connection object only triggers that callback,
            // not every callback connected to its ControlObject.
            "var connection2 = engine.makeConnection('[Test]', 'co', incrementCounterCO);"
            "connection1.trigger();"));
    // The counter should have been incremented exactly once.
    EXPECT_DOUBLE_EQ(1.0, counter->get());
}

TEST_F(ControllerScriptEngineLegacyTest, connectionExecutesWithCorrectThisObject) {
    // Test that callback functions are executed with JavaScript's
    // 'this' keyword referring to the object in which the connection
    // was created.
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    auto pass = std::make_unique<ControlObject>(ConfigKey("[Test]", "passed"));

    EXPECT_TRUE(evaluateAndAssert(
            "var TestObject = function () {"
            "  this.executeTheCallback = true;"
            "  this.connection = engine.makeConnection('[Test]', 'co', function () {"
            "    if (this.executeTheCallback) {"
            "      engine.setValue('[Test]', 'passed', 1);"
            "    }"
            "  }.bind(this));"
            "};"
            "var someObject = new TestObject();"
            "someObject.connection.trigger();"));
    // ControlObjectScript connections are processed via QueuedConnection. Use
    // processEvents() to cause Qt to deliver them.
    processEvents();
    // The counter should have been incremented exactly once.
    EXPECT_DOUBLE_EQ(1.0, pass->get());
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0) // Latin9 is available form Qt 6.5
TEST_F(ControllerScriptEngineLegacyTest, convertCharsetCorrectValueStringCharset) {
    const auto result = evaluate(
            "engine.convertCharset(engine.Charset.Latin9, 'Hello! €')");

    EXPECT_EQ(qjsvalue_cast<QByteArray>(result),
            QByteArrayView::fromArray({'\x48',
                    '\x65',
                    '\x6c',
                    '\x6c',
                    '\x6f',
                    '\x21',
                    '\x20',
                    '\xA4'}));
}

TEST_F(ControllerScriptEngineLegacyTest, convertCharsetUnsupportedChars) {
    auto result = qjsvalue_cast<QByteArray>(
            evaluate("engine.convertCharset(engine.Charset.Latin9, 'مايأ نامز ™')"));
    char sub = '\x1A'; // ASCII/Latin9 SUB character
    EXPECT_EQ(result,
            QByteArrayView::fromArray(
                    {sub, sub, sub, sub, '\x20', sub, sub, sub, sub, '\x20', sub}));
}
#endif

TEST_F(ControllerScriptEngineLegacyTest, convertCharsetLatin1Eur) {
    const auto result = evaluate(
            "engine.convertCharset(engine.Charset.Latin1, 'Hello! ¤€')");

    char sub = '?'; // used by Qt for substitution
    EXPECT_EQ(qjsvalue_cast<QByteArray>(result),
            QByteArrayView::fromArray({'\x48',
                    '\x65',
                    '\x6c',
                    '\x6c',
                    '\x6f',
                    '\x21',
                    '\x20',
                    '\xA4',
                    sub}));
}

TEST_F(ControllerScriptEngineLegacyTest, convertCharsetMultiByteEncoding) {
    auto result = qjsvalue_cast<QByteArray>(
            evaluate("engine.convertCharset(engine.Charset.UTF_16LE, 'مايأ نامز')"));
    EXPECT_EQ(result,
            QByteArrayView::fromArray({'\x45',
                    '\x06',
                    '\x27',
                    '\x06',
                    '\x4A',
                    '\x06',
                    '\x23',
                    '\x06',
                    '\x20',
                    '\x00',
                    '\x46',
                    '\x06',
                    '\x27',
                    '\x06',
                    '\x45',
                    '\x06',
                    '\x32',
                    '\x06'}));
}

#define COMPLICATEDSTRINGLITERAL "Hello, 世界! שלום! こんにちは! 안녕하세요! ™ 😊"

static int convertedCharsetForString(ControllerScriptInterfaceLegacy::Charset charset) {
    // the expected length after conversion of COMPLICATEDSTRINGLITERAL
    using enum ControllerScriptInterfaceLegacy::Charset;

#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    switch (charset) {
    case UTF_8:
        return 67;
    case UTF_16LE:
    case UTF_16BE:
        return 70;
    case UTF_32LE:
    case UTF_32BE:
        return 136;
    case ASCII:
    case CentralEurope:
    case Cyrillic:
    case WesternEurope:
    case Greek:
    case Turkish:
    case Hebrew:
    case Arabic:
    case Baltic:
    case Vietnamese:
    case Latin9:
    case KOI8_U:
        return 34;
    case Latin1:
        // Latin1 is handled by Qt internally and 😊 becomes "??"
        return 35;
    case EUC_JP:
        return 53;
    case Shift_JIS:
    case EUC_KR:
    case Big5_HKSCS:
        return 52;
    case UCS2:
        return 72;
    case SCSU:
        return 55;
    case BOCU_1:
        return 56;
    case CESU_8:
        return 69;
    }
#else
    // Qt < 6.4 only supports these conversions
    switch (charset) {
    case UTF_8:
        return 67;
    case UTF_16LE:
    case UTF_16BE:
        return 70;
    case UTF_32LE:
    case UTF_32BE:
        return 136;
    case Latin1:
        return 35;
    default:
        return 0;
    }
#endif

    // unreachable, but gtest does not offer a way to assert this here.
    // returning 0 will almost certainly also result in a failure.
    return 0;
}

TEST_F(ControllerScriptEngineLegacyTest, convertCharsetAllCharset) {
    QMetaEnum charsetEnumEntry = QMetaEnum::fromType<
            ControllerScriptInterfaceLegacy::Charset>();

    for (int i = 0; i < charsetEnumEntry.keyCount(); ++i) {
        QString key = charsetEnumEntry.key(i);
        auto enumValue =
                static_cast<ControllerScriptInterfaceLegacy::Charset>(
                        charsetEnumEntry.value(i));
        QString source = QStringLiteral(
                "engine.convertCharset(engine.Charset.%1, "
                "'" COMPLICATEDSTRINGLITERAL "')")
                                 .arg(key);
        auto result = qjsvalue_cast<QByteArray>(evaluate(source));
        EXPECT_EQ(result.size(), convertedCharsetForString(enumValue))
                << "Unexpected length of converted string for encoding: '"
                << key.toStdString() << "'";
    }
}

TEST_F(ControllerScriptEngineLegacyTest, JavascriptPlayerProxy) {
    QMap<QString, QString> expectedValues = {
            std::pair("artist", "Test Artist"),
            std::pair("title", "Test title"),
            std::pair("album", "Test Album"),
            std::pair("albumArtist", "Test Album Artist"),
            std::pair("genre", "Test genre"),
            std::pair("composer", "Test Composer"),
            std::pair("grouping", ""),
            std::pair("year", "2011"),
            std::pair("trackNumber", "07"),
            std::pair("trackTotal", "60")};

    m_pJSEngine->globalObject().setProperty(
            "testedValues", m_pJSEngine->toScriptValue(expectedValues.keys()));

    const auto* code =
            "var result = {};"
            "var player = engine.getPlayer('[Channel1]');"
            "for(const name of testedValues) {"
            "    player[`${name}Changed`].connect(newValue => {"
            "        result[name] = newValue;"
            "    });"
            "}";

    EXPECT_TRUE(evaluateAndAssert(code)) << "Evaluation error in test code";
    loadTrackSync("id3-test-data/all.mp3");
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    for (auto [property, expected] : expectedValues.asKeyValueRange()) {
#else
    for (auto it = expectedValues.constBegin(); it != expectedValues.constEnd(); ++it) {
        const QString& property = it.key();
        const QString& expected = it.value();
#endif
        auto const playerActual = evaluate("player." + property).toString();
        auto const slotActual = evaluate("result." + property).toString();
        EXPECT_QSTRING_EQ(expected, playerActual)
                << QString("engine.getPlayer(...).%1 doesn't corresponds to "
                           "its expected value (expected: %2, actual: %3)")
                           .arg(property, expected, playerActual)
                           .toStdString();
        EXPECT_QSTRING_EQ(expected, slotActual) << QString(
                "engine.getPlayer(...).%1Changed slot didn't produce the "
                "expected value (expected: %2, actual: %3)")
                                                           .arg(property, expected, playerActual)
                                                           .toStdString();
    }
}

#ifdef MIXXX_USE_QML
class MockScreenRender : public ControllerRenderingEngine {
  public:
    MockScreenRender(const LegacyControllerMapping::ScreenInfo& info)
            : ControllerRenderingEngine(info, new ControllerEngineThreadControl) {};
    MOCK_METHOD(void,
            requestSendingFrameData,
            (Controller * controller, const QByteArray& frame),
            (override));
};

TEST_F(ControllerScriptEngineLegacyTest, screenWontSentRawDataIfNotConfigured) {
    LogCaptureGuard logCaptureGuard;
    LegacyControllerMapping::ScreenInfo dummyScreen{
            "",                                                    // identifier
            QSize(0, 0),                                           // size
            10,                                                    // target_fps
            1,                                                     // msaa
            std::chrono::milliseconds(10),                         // splash_off
            QImage::Format_RGB16,                                  // pixelFormat
            LegacyControllerMapping::ScreenInfo::ColorEndian::Big, // endian
            false,                                                 // rawData
            false                                                  // reversedColor
    };
    QImage dummyFrame;
    // Allocate screen on the heap as it need to outlive the this function,
    // since the engine will take ownership of it
    std::shared_ptr<MockScreenRender> pDummyRender =
            std::make_shared<MockScreenRender>(dummyScreen);
    EXPECT_CALL(*pDummyRender, requestSendingFrameData(_, _)).Times(0);
    EXPECT_LOG_MSG(QtWarningMsg,
            "Could not find a valid transform function but the screen doesn't "
            "accept raw data. Aborting screen rendering.");

    renderingScreens().insert(dummyScreen.identifier, pDummyRender);
    rootItems().emplace(dummyScreen.identifier,
            std::make_unique<mixxx::qml::QmlMixxxControllerScreen>());

    testHandleScreen(
            dummyScreen,
            dummyFrame,
            QDateTime::currentDateTime());

    ASSERT_ALL_EXPECTED_MSG();
}

TEST_F(ControllerScriptEngineLegacyTest, screenWillSentRawDataIfConfigured) {
    LogCaptureGuard logCaptureGuard;
    LegacyControllerMapping::ScreenInfo dummyScreen{
            "",                                                    // identifier
            QSize(0, 0),                                           // size
            10,                                                    // target_fps
            1,                                                     // msaa
            std::chrono::milliseconds(10),                         // splash_off
            QImage::Format_RGB16,                                  // pixelFormat
            LegacyControllerMapping::ScreenInfo::ColorEndian::Big, // endian
            false,                                                 // reversedColor
            true                                                   // rawData
    };
    QImage dummyFrame;
    // Allocate screen on the heap as it need to outlive the this function,
    // since the engine will take ownership of it
    std::shared_ptr<MockScreenRender> pDummyRender =
            std::make_shared<MockScreenRender>(dummyScreen);
    EXPECT_CALL(*pDummyRender, requestSendingFrameData(_, QByteArray()));

    renderingScreens().insert(dummyScreen.identifier, pDummyRender);
    rootItems().emplace(dummyScreen.identifier,
            std::make_unique<mixxx::qml::QmlMixxxControllerScreen>());

    testHandleScreen(
            dummyScreen,
            dummyFrame,
            QDateTime::currentDateTime());

    ASSERT_ALL_EXPECTED_MSG();
}
#endif

TEST_F(ControllerScriptEngineLegacyTimerTest, beginTimer_repeatedTimer) {
    EXPECT_TRUE(evaluateAndAssert(
            "engine.setValue('[Test]', 'co', 0.0);"));
    EXPECT_DOUBLE_EQ(0.0, co->get());

    EXPECT_TRUE(
            evaluateAndAssert(R"(engine.beginTimer(50, function() {
                                    let x = engine.getValue('[Test]', 'co');
                                    x++; 
                                    engine.setValue('[Test]', 'co', x);
                                 }, false);)"));
    processEvents();
    EXPECT_DOUBLE_EQ(0.0, co->get());

    jsEngine()->thread()->msleep(70);
    processEvents();

    EXPECT_DOUBLE_EQ(1.0, co->get());

    jsEngine()->thread()->msleep(140);
    processEvents();

    EXPECT_DOUBLE_EQ(2.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTimerTest, beginTimer_singleShotTimer) {
    EXPECT_TRUE(evaluateAndAssert(
            "engine.setValue('[Test]', 'co', 0.0);"));
    EXPECT_DOUBLE_EQ(0.0, co->get());

    // Single shot timer with minimum allowed interval of 20ms
    EXPECT_TRUE(evaluateAndAssert(
            R"(engine.beginTimer(20, function() {
                   engine.setValue('[Test]', 'co', 1.0);
               }, true);)"));
    processEvents();
    EXPECT_DOUBLE_EQ(0.0, co->get());

    jsEngine()->thread()->msleep(35);
    processEvents();

    EXPECT_DOUBLE_EQ(1.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTimerTest, beginTimer_singleShotTimerBindFunction) {
    // Single shot timer with minimum allowed interval of 20ms
    EXPECT_TRUE(evaluateAndAssert(
            R"(var globVar = 7;
            timerId = engine.beginTimer(20, function () {
                engine.setValue('[Test]', 'co', this.globVar);
                this.globVar++;
                engine.setValue('[Test]', 'coTimerId', timerId + 10);
            }.bind(this), true);            
            engine.setValue('[Test]', 'coTimerId', timerId);)"));
    processEvents();
    EXPECT_DOUBLE_EQ(0.0, co->get());
    double timerId = coTimerId->get();
    EXPECT_TRUE(timerId > 0);

    jsEngine()->thread()->msleep(35);
    processEvents();

    EXPECT_DOUBLE_EQ(timerId + 10, coTimerId->get());
    EXPECT_DOUBLE_EQ(7.0, co->get());
    EXPECT_TRUE(evaluateAndAssert("engine.setValue('[Test]', 'co', this.globVar);"));
    processEvents();

    EXPECT_DOUBLE_EQ(8.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTimerTest, beginTimer_singleShotTimerArrowFunction) {
    // Single shot timer with minimum allowed interval of 20ms
    EXPECT_TRUE(evaluateAndAssert(
            R"(var globVar = 7;
            timerId = engine.beginTimer(20, () => {
                engine.setValue('[Test]', 'co', this.globVar);
                this.globVar++;
                engine.setValue('[Test]', 'coTimerId', timerId + 10);
            }, true);            
            engine.setValue('[Test]', 'coTimerId', timerId);)"));
    processEvents();
    EXPECT_DOUBLE_EQ(0.0, co->get());
    double timerId = coTimerId->get();
    EXPECT_TRUE(timerId > 0);

    jsEngine()->thread()->msleep(35);
    processEvents();

    EXPECT_DOUBLE_EQ(timerId + 10, coTimerId->get());
    EXPECT_DOUBLE_EQ(7.0, co->get());
    EXPECT_TRUE(evaluateAndAssert("engine.setValue('[Test]', 'co', this.globVar);"));
    processEvents();

    EXPECT_DOUBLE_EQ(8.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTimerTest, beginTimer_singleShotTimerBindFunctionInClass) {
    // Single shot timer with minimum allowed interval of 20ms
    EXPECT_TRUE(evaluateAndAssert(
            R"(
            class MyClass {
               constructor() {
                  this.timerId = undefined;
                  this.globVar = 7;
               }
               runTimer() {
                  this.timerId = engine.beginTimer(20, function() {
                     engine.setValue('[Test]', 'co', this.globVar);
                     this.globVar++;
                     engine.setValue('[Test]', 'coTimerId', this.timerId + 10);
                  }.bind(this), true);            
                  engine.setValue('[Test]', 'coTimerId', this.timerId);
               }
            }
            var MyMapping = new MyClass();
            MyMapping.runTimer();)"));
    processEvents();
    EXPECT_DOUBLE_EQ(0.0, co->get());
    double timerId = coTimerId->get();
    EXPECT_TRUE(timerId > 0);

    jsEngine()->thread()->msleep(35);
    processEvents();

    EXPECT_DOUBLE_EQ(timerId + 10, coTimerId->get());
    EXPECT_DOUBLE_EQ(7.0, co->get());
    EXPECT_TRUE(evaluateAndAssert("engine.setValue('[Test]', 'co', MyMapping.globVar);"));
    processEvents();

    EXPECT_DOUBLE_EQ(8.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTimerTest, beginTimer_singleShotTimerArrowFunctionInClass) {
    EXPECT_TRUE(evaluateAndAssert(
            R"(
            class MyClass {
               constructor() {
                  this.timerId = undefined;
                  this.globVar = 7;
               }
               runTimer() {                  
                  const savedThis = this;
                  this.timerId = engine.beginTimer(20, () => {
                     if (savedThis !== this) { throw new Error("savedThis should be equal to this"); }
                     if (!(this instanceof MyClass)) { throw new Error("this should be an instance of MyClass"); }
                     engine.setValue('[Test]', 'co', this.globVar);
                     this.globVar++;
                     engine.setValue('[Test]', 'coTimerId', this.timerId + 10);
                  }, true);            
                  engine.setValue('[Test]', 'coTimerId', this.timerId);
               }
            }
            var MyMapping = new MyClass();
            MyMapping.runTimer();)"));
    processEvents();
    EXPECT_DOUBLE_EQ(0.0, co->get());
    double timerId = coTimerId->get();
    EXPECT_TRUE(timerId > 0);

    jsEngine()->thread()->msleep(35);
    processEvents();

    EXPECT_DOUBLE_EQ(timerId + 10, coTimerId->get());
    EXPECT_DOUBLE_EQ(7.0, co->get());
    EXPECT_TRUE(evaluateAndAssert("engine.setValue('[Test]', 'co', MyMapping.globVar);"));
    processEvents();

    EXPECT_DOUBLE_EQ(8.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTimerTest, beginTimer_repeatedTimerArrowFunctionCallInClass) {
    // Single shot timer with minimum allowed interval of 20ms
    EXPECT_TRUE(evaluateAndAssert(
            R"(
            class MyClass {
               constructor() {
                  this.timerId = undefined;
                  this.globVar = 7;
               }
               stopTimer() {
                  if (!(this instanceof MyClass)) { throw new Error("this should be an instance of MyClass"); }
                  engine.stopTimer(this.timerId);
                  this.timerId = 0;
                  engine.setValue('[Test]', 'coTimerId', this.timerId + 20);
               }
               runTimer() {
                  this.timerId = engine.beginTimer(20, () => this.stopTimer(), false);                  
                  engine.setValue('[Test]', 'co', this.globVar);      
                  engine.setValue('[Test]', 'coTimerId', this.timerId);
               }
            }
            var MyMapping = new MyClass();
            MyMapping.runTimer();)"));
    processEvents();
    EXPECT_DOUBLE_EQ(7.0, co->get());
    double timerId = coTimerId->get();
    EXPECT_TRUE(timerId > 0);

    jsEngine()->thread()->msleep(35);
    processEvents();

    EXPECT_DOUBLE_EQ(20, coTimerId->get());

    jsEngine()->thread()->msleep(35);
    processEvents();

    EXPECT_DOUBLE_EQ(20, coTimerId->get());
}
TEST_F(ControllerScriptEngineLegacyTimerTest, beginTimer_repeatedTimerThisFunctionCallInClass) {
    // Single shot timer with minimum allowed interval of 20ms
    EXPECT_TRUE(evaluateAndAssert(
            R"(
            class MyClass {
               constructor() {
                  this.timerId = undefined;
                  this.globVar = 7;
               }
               stopTimer() {    
                  if (!(this instanceof MyClass)) {throw new Error("this should be an instance of MyClass");}
                  engine.stopTimer(this.timerId);
                  this.timerId = 0;
                  engine.setValue('[Test]', 'coTimerId', this.timerId + 20);
               }
               runTimer() {
                  this.timerId = engine.beginTimer(20, this.stopTimer.bind(this), false);              
                  engine.setValue('[Test]', 'co', this.globVar);
                  engine.setValue('[Test]', 'coTimerId', this.timerId);
               }
            }
            var MyMapping = new MyClass();
            MyMapping.runTimer();)"));
    processEvents();
    EXPECT_DOUBLE_EQ(7.0, co->get());
    double timerId = coTimerId->get();
    EXPECT_TRUE(timerId > 0);

    jsEngine()->thread()->msleep(35);
    processEvents();

    EXPECT_DOUBLE_EQ(20, coTimerId->get());

    jsEngine()->thread()->msleep(35);
    processEvents();

    EXPECT_DOUBLE_EQ(20, coTimerId->get());
}
