#ifndef BACKENDS_AHI_H
#define BACKENDS_AHI_H

#include "backends/base.h"

struct AHIBackendFactory final : public BackendFactory {
public:
    bool init() override;
    
    bool querySupport(BackendType type) override;
    
    void probe(DevProbe type, std::string *outnames) override;

    BackendPtr createBackend(ALCdevice *device, BackendType type) override;

    static BackendFactory &getFactory();
};

#endif /* BACKENDS_AHI_H */
