#include "MesaDRM.hpp"
#include <algorithm>
#include <xf86drm.h>
#include "../Compositor.hpp"
#include "types/WLBuffer.hpp"
#include "../render/OpenGL.hpp"

CMesaDRMBufferResource::CMesaDRMBufferResource(uint32_t id, wl_client* client, Aquamarine::SDMABUFAttrs attrs_) {
    LOGM(LOG, "Creating a Mesa dmabuf, with id {}: size {}, fmt {}, planes {}", id, attrs_.size, attrs_.format, attrs_.planes);
    for (int i = 0; i < attrs_.planes; ++i) {
        LOGM(LOG, " | plane {}: mod {} fd {} stride {} offset {}", i, attrs_.modifier, attrs_.fds[i], attrs_.strides[i], attrs_.offsets[i]);
    }

    m_buffer                       = makeShared<CDMABuffer>(id, client, attrs_);
    m_buffer->m_resource->m_buffer = m_buffer;

    m_listeners.bufferResourceDestroy = m_buffer->events.destroy.listen([this] {
        m_listeners.bufferResourceDestroy.reset();
        PROTO::mesaDRM->destroyResource(this);
    });

    if (!m_buffer->m_success)
        LOGM(ERR, "Possibly compositor bug: buffer failed to create");
}

CMesaDRMBufferResource::~CMesaDRMBufferResource() {
    if (m_buffer && m_buffer->m_resource)
        m_buffer->m_resource->sendRelease();
    m_buffer.reset();
    m_listeners.bufferResourceDestroy.reset();
}

bool CMesaDRMBufferResource::good() {
    return m_buffer && m_buffer->good();
}

CMesaDRMResource::CMesaDRMResource(SP<CWlDrm> resource_) : m_resource(resource_) {
    if UNLIKELY (!good())
        return;

    m_resource->setOnDestroy([this](CWlDrm* r) { PROTO::mesaDRM->destroyResource(this); });

    m_resource->setAuthenticate([this](CWlDrm* r, uint32_t token) {
        // we don't need this
        m_resource->sendAuthenticated();
    });

    m_resource->setCreateBuffer(
        [](CWlDrm* r, uint32_t, uint32_t, int32_t, int32_t, uint32_t, uint32_t) { r->error(WL_DRM_ERROR_INVALID_NAME, "Not supported, use prime instead"); });

    m_resource->setCreatePlanarBuffer([](CWlDrm* r, uint32_t, uint32_t, int32_t, int32_t, uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) {
        r->error(WL_DRM_ERROR_INVALID_NAME, "Not supported, use prime instead");
    });

    m_resource->setCreatePrimeBuffer(
        [this](CWlDrm* r, uint32_t id, int32_t nameFd, int32_t w, int32_t h, uint32_t fmt, int32_t off0, int32_t str0, int32_t off1, int32_t str1, int32_t off2, int32_t str2) {
            if (off0 < 0 || w <= 0 || h <= 0) {
                r->error(WL_DRM_ERROR_INVALID_FORMAT, "Invalid w, h, or offset");
                return;
            }

            uint64_t mod = DRM_FORMAT_MOD_INVALID;

            auto     fmts = g_pHyprOpenGL->getDRMFormats();
            for (auto const& f : fmts) {
                if (f.drmFormat != fmt)
                    continue;

                for (auto const& m : f.modifiers) {
                    if (m == DRM_FORMAT_MOD_LINEAR)
                        continue;

                    mod = m;
                    break;
                }
                break;
            }

            Aquamarine::SDMABUFAttrs attrs;
            attrs.success    = true;
            attrs.size       = {w, h};
            attrs.modifier   = mod;
            attrs.planes     = 1;
            attrs.offsets[0] = off0;
            attrs.strides[0] = str0;
            attrs.fds[0]     = nameFd;
            attrs.format     = fmt;

            const auto RESOURCE = PROTO::mesaDRM->m_buffers.emplace_back(makeShared<CMesaDRMBufferResource>(id, m_resource->client(), attrs));

            if UNLIKELY (!RESOURCE->good()) {
                r->noMemory();
                PROTO::mesaDRM->m_buffers.pop_back();
                return;
            }

            // append instance so that buffer knows its owner
            RESOURCE->m_buffer->m_resource->m_buffer = RESOURCE->m_buffer;
        });

    m_resource->sendDevice(PROTO::mesaDRM->m_nodeName.c_str());
    m_resource->sendCapabilities(WL_DRM_CAPABILITY_PRIME);

    auto fmts = g_pHyprOpenGL->getDRMFormats();
    for (auto const& fmt : fmts) {
        m_resource->sendFormat(fmt.drmFormat);
    }
}

bool CMesaDRMResource::good() {
    return m_resource->resource();
}

CMesaDRMProtocol::CMesaDRMProtocol(const wl_interface* iface, const int& ver, const std::string& name) : IWaylandProtocol(iface, ver, name) {
    drmDevice* dev   = nullptr;
    int        drmFD = g_pCompositor->m_drmFD;
    if (drmGetDevice2(drmFD, 0, &dev) != 0) {
        LOGM(ERR, "Failed to get device, disabling MesaDRM");
        removeGlobal();
        return;
    }

    if (dev->available_nodes & (1 << DRM_NODE_RENDER)) {
        m_nodeName = dev->nodes[DRM_NODE_RENDER];
    } else {
        ASSERT(dev->available_nodes & (1 << DRM_NODE_PRIMARY));

        if (!dev->nodes[DRM_NODE_PRIMARY]) {
            LOGM(ERR, "No DRM render node available, both render and primary are null, disabling MesaDRM");
            drmFreeDevice(&dev);
            removeGlobal();
            return;
        }

        LOGM(WARN, "No DRM render node, falling back to primary {}", dev->nodes[DRM_NODE_PRIMARY]);
        m_nodeName = dev->nodes[DRM_NODE_PRIMARY];
    }
    drmFreeDevice(&dev);
}

void CMesaDRMProtocol::bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id) {
    const auto RESOURCE = m_managers.emplace_back(makeShared<CMesaDRMResource>(makeShared<CWlDrm>(client, ver, id)));

    if UNLIKELY (!RESOURCE->good()) {
        wl_client_post_no_memory(client);
        m_managers.pop_back();
        return;
    }
}

void CMesaDRMProtocol::destroyResource(CMesaDRMResource* resource) {
    std::erase_if(m_managers, [&](const auto& other) { return other.get() == resource; });
}

void CMesaDRMProtocol::destroyResource(CMesaDRMBufferResource* resource) {
    std::erase_if(m_buffers, [&](const auto& other) { return other.get() == resource; });
}
