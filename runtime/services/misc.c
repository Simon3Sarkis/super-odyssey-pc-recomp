// Small services: parental control, add-on content, and other mostly-stubbed interfaces.
#include <string.h>

#include "../hle.h"
#include "../ipc.h"

static void ok_logger(Service *s, IpcReq *req, IpcResp *resp) {
    LOG("[svc] %s cmd %u (ok)\n", s->name, req->cmd);
    resp_size(resp, 0x10);
}

static uint32_t new_event_handle(void) {
    uint32_t w, r;
    kernel_create_event_pair(&w, &r);
    return r;
}

// ------------------------------------------------------------------ pctl
static void pctl_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1031: case 1035: case 1018: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;  // restrictions off
    case 1032: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;
    default: LOG("[pctl] IParentalControlService cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

static void pctl_root(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 0 || req->cmd == 1)
        ipc_return_object(resp, service_new("IParentalControlService", pctl_service, NULL));
}
SERVICE("pctl", pctl) { pctl_root(s, req, resp); }
SERVICE("pctl:s", pctl_s) { pctl_root(s, req, resp); }
SERVICE("pctl:a", pctl_a) { pctl_root(s, req, resp); }
SERVICE("pctl:r", pctl_r) { pctl_root(s, req, resp); }

// ------------------------------------------------------------------ bcat:u (no delivered content)
// Odyssey 1.4.1 mounts its BCAT delivery cache at boot and lists its directories (nn::bcat::
// EnumerateDeliveryCacheDirectory); the SDK aborts if the storage it opened isn't an object. As a console that has
// downloaded nothing: real objects, an empty directory list.
static void bcat_storage(Service *s, IpcReq *req, IpcResp *resp) {
    switch (req->cmd) {
    case 0: ipc_return_object(resp, service_new("IDeliveryCacheFileService", ok_logger, NULL)); break;
    case 1: ipc_return_object(resp, service_new("IDeliveryCacheDirectoryService", ok_logger, NULL)); break;
    case 10: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;  // EnumerateDeliveryCacheDirectory: none
    default: ok_logger(s, req, resp);
    }
}
SERVICE("bcat:u", bcat_u) {
    switch (req->cmd) {
    case 0: ipc_return_object(resp, service_new("IBcatService", ok_logger, NULL)); break;  // CreateBcatService
    case 1: case 2:  // CreateDeliveryCacheStorageService (/ WithApplicationId)
        ipc_return_object(resp, service_new("IDeliveryCacheStorageService", bcat_storage, NULL));
        break;
    default: ok_logger(s, req, resp);
    }
}

// ------------------------------------------------------------------ aoc:u (no DLC)
SERVICE("aoc:u", aoc_u) {
    (void)s;
    switch (req->cmd) {
    case 2: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;   // CountAddOnContent
    case 3: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;   // ListAddOnContent
    case 5: OUT(uint64_t, 0) = 0x01006BB00C6F1000ull; resp_size(resp, 8); break;  // GetAddOnContentBaseId
    case 8: resp->copy_handles[resp->ncopy++] = new_event_handle(); break;
    default: ok_logger(s, req, resp);
    }
}
