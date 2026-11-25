#include "fpp-pch.h"

#include <atomic>
#include <sys/stat.h>

#include "common.h"
#include "log.h"
#include "Warnings.h"
#include "settings.h"
#include "overlays/PixelOverlay.h"
#include "overlays/PixelOverlayModel.h"

#include "TmcUtil.h"
#include "TmcOutput.h"

namespace tmc {

// The one instance feeding the steppers. A second entry of this type on the
// Channel Outputs page is refused: Init() rebuilds the manager's channel
// table, which the active instance reads on the output thread without a lock
// (TmcManager::distribute()). FPP closes the outputs it replaces before it
// initialises the new ones, so a reload is not a second instance.
static std::atomic<TmcOutput*> s_active { nullptr };

TmcOutput::TmcOutput(unsigned int startChannel, unsigned int channelCount) :
    ChannelOutput(startChannel, channelCount) {
    LogDebug(VB_PLUGIN, "%s: TmcOutput(%u, %u)\n", LOGPFX, startChannel,
             channelCount);
}

TmcOutput::~TmcOutput() {
    TmcOutput* self = this;
    s_active.compare_exchange_strong(self, nullptr);
}

int TmcOutput::Init(Json::Value config) {
    TmcOutput* none = nullptr;
    if (!s_active.compare_exchange_strong(none, this)) {
        LogErr(VB_PLUGIN, "%s: a second %s channel output was refused: the plugin "
               "needs exactly one (Channel Outputs, Misc tab); delete the extra one\n",
               LOGPFX, PLUGIN_NAME);
        return 0;
    }
    // The axes are already running: the plugin brings them up when it loads,
    // so commissioning works with no channel output configured at all. All
    // this output adds is the feed of live channel values.
    TmcManager::instance().resolveChannels();
    int n = TmcManager::instance().axisCount();
    LogInfo(VB_PLUGIN,
            "%s: channel output attached to %d steppers, data origin channel %u\n",
            LOGPFX, n, m_startChannel + 1);
    if (n == 0) {
        LogWarn(VB_PLUGIN,
                "%s: no steppers are configured or running -- channel data will be "
                "ignored until at least one comes up\n",
                LOGPFX);
    }
    return ChannelOutput::Init(config);
}

int TmcOutput::Close() {
    LogDebug(VB_PLUGIN, "%s: TmcOutput::Close()\n", LOGPFX);
    TmcManager::instance().showStopping();
    TmcOutput* self = this;
    s_active.compare_exchange_strong(self, nullptr);
    return ChannelOutput::Close();
}

// One range per stepper, not one span covering them all: two steppers can
// sit at opposite ends of the show, and claiming everything in between would
// have FPP load channel data nothing reads.
void TmcOutput::GetRequiredChannelRanges(const std::function<void(int, int)>& addRange) {
    TmcManager& mgr = TmcManager::instance();
    std::vector<int> resolved = mgr.resolvedChannels();
    bool any = false;
    for (int ch : resolved) {
        if (ch > 0) {
            addRange(ch - 1, ch - 1); // ranges are 0-based
            any = true;
        }
    }
    // Nothing resolved (every stepper's model is missing): claim the one
    // channel the output entry itself declares, so FPP still has a range.
    if (!any)
        addRange(m_startChannel, m_startChannel + m_channelCount - 1);
}

void TmcOutput::StartingOutput() {
    TmcManager::instance().showStarting();
}

void TmcOutput::StoppingOutput() {
    // Also called when FPP reloads its outputs, so this only wakes the axes;
    // the idle timeout decides that the show has ended.
    TmcManager::instance().showStopping();
}

int TmcOutput::SendData(unsigned char* channelData) {
    // channelData is already offset to this output's start channel, so that is
    // the origin an absolute channel is measured from.
    TmcManager::instance().distribute(channelData, (int)m_startChannel,
                                      (int)m_channelCount);
    return m_channelCount;
}

void TmcOutput::DumpConfig() {
    LogDebug(VB_PLUGIN, "%s: start=%u count=%u axes=%d\n", LOGPFX, m_startChannel,
             m_channelCount, TmcManager::instance().axisCount());
    ChannelOutput::DumpConfig();
}

} // namespace tmc
