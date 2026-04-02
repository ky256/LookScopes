#pragma once

#include "ofxsImageEffect.h"

class NDIReceiverPluginFactory : public OFX::PluginFactoryHelper<NDIReceiverPluginFactory>
{
public:
    NDIReceiverPluginFactory();
    virtual void load() override;
    virtual void unload() override;
    virtual void describe(OFX::ImageEffectDescriptor& p_Desc) override;
    virtual void describeInContext(OFX::ImageEffectDescriptor& p_Desc, OFX::ContextEnum p_Context) override;
    virtual OFX::ImageEffect* createInstance(OfxImageEffectHandle p_Handle, OFX::ContextEnum p_Context) override;
};
