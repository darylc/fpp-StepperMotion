#include "fpp-pch.h"

#include "AxisWorker.h"
#include "TmcApi.h"
#include "TmcCommands.h"
#include "TmcManager.h"
#include "TmcOutput.h"
#include "TmcUtil.h"

#include "Plugin.h"
#include "log.h"

/*
 * Plugin entry point.
 *
 * The name (PLUGIN_NAME, TmcUtil.h) is load-bearing three times over, and all
 * three have to agree: it is this object's name, this plugin's DIRECTORY
 * name, and the "type" written into co-other.json for the channel output.
 *
 * Why the directory name specifically: fppd builds channel outputs BEFORE it
 * loads user plugins, so when ChannelOutputSetup resolves our output it
 * calls findPlugin("<type>", "fpp-co-<type>") with nothing of ours loaded
 * yet. That first tries dlopen("libfpp-co-<type>.so") by bare name, which
 * cannot work from a plugin directory (fppd's RUNPATH covers $ORIGIN and
 * /opt/fpp/src, not media/plugins). It then falls back to: if
 * plugins/<type>/ exists, load it as a user plugin (callbacks.sh names the
 * library) and look again by name. That fallback is what brings us up, and
 * it keys on the directory -- so the type must be the directory name, and
 * this object must answer to it.
 *
 * The two "Failed to load shlib libfpp-co-<name>.so" lines
 * in the log just before we load are that first dlopen attempt failing:
 * expected and harmless.
 */
class TmcPlugin : public FPPPlugins::Plugin,
                  public FPPPlugins::ChannelOutputPlugin,
                  public FPPPlugins::APIProviderPlugin {
public:
    // The configuration file (config/plugin.<name>.json) is written by the
    // plugin's saves, and also by FPP's backup restore or a hand edit: the
    // manager watches it and picks those up (TmcManager::watchOutputEntry()).
    TmcPlugin() :
        FPPPlugins::Plugin(tmc::PLUGIN_NAME) {
        // Axes come up with the plugin rather than the channel output, so an
        // axis can be commissioned before any output is configured.
        tmc::TmcManager::instance().reloadConfigFile();
        tmc::TmcManager::instance().startAxes();
        tmc::registerTmcCommands();
        // FPP's channel output entry for the plugin, in step with the channel
        // block now and whenever co-other.json changes. fppd has loaded its
        // pixel overlay models before any plugin, so a block on a model
        // resolves here; the requests finish once fppd's main loop runs.
        tmc::TmcManager::instance().watchOutputEntry();
        tmc::TmcManager::instance().syncOutputEntry(true);
    }

    ~TmcPlugin() override {
    }

    ChannelOutput* createChannelOutput(unsigned int startChannel,
                                       unsigned int channelCount) override {
        return new tmc::TmcOutput(startChannel, channelCount);
    }

    // `this` is the owner token: a retired instance must not disarm the route
    // its replacement has already claimed.
    void registerApis() override { tmc::registerTmcApis(this); }
    void unregisterApis() override { tmc::unregisterTmcApis(this); }

    std::function<bool()> shutdown() override {
        tmc::unregisterTmcCommands();
        tmc::TmcManager::instance().unwatchOutputEntry();
        // Joins every stepper worker before the object is destroyed.
        tmc::TmcManager::instance().stopAxes();
        return nullptr;
    }
};

/*
 * No FPP_PLUGIN_SUPPORTS_UNLOAD(): a plugin that produces a channel output
 * is pinned by noteChannelOutputCreated() and refused a runtime unload.
 */
extern "C" {
FPPPlugins::Plugin* createPlugin() {
    return new TmcPlugin();
}
}
