#pragma once
/*
 * TmcOutput is the FPP channel output that feeds the axes TmcManager owns:
 * one byte per stepper, handed to the manager on every frame.
 */

#include "channeloutput/ChannelOutput.h"

#include "TmcManager.h"
#include "TmcUtil.h"

namespace tmc {

class TmcOutput : public ChannelOutput {
public:
    TmcOutput(unsigned int startChannel, unsigned int channelCount);
    ~TmcOutput() override;

    // The same name as the co-other "type" and the plugin directory. FPP
    // only uses this as a label.
    std::string GetOutputType() const override { return PLUGIN_NAME; }

    int Init(Json::Value config) override;
    int Close() override;

    int SendData(unsigned char* channelData) override;
    void GetRequiredChannelRanges(const std::function<void(int, int)>& addRange) override;

    void StartingOutput() override;
    void StoppingOutput() override;

protected:
    void DumpConfig() override;
};

} // namespace tmc
