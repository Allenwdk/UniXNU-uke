/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Production release/stop/unwind functions are inserted by the checker. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <vector>

static unsigned checks;
#define CHECK(value) do {++checks; assert(value);} while (0)
static std::vector<unsigned> events;
struct Buffer {
    unsigned pas, completed = 0, freed = 0;
    void complete() {++completed; events.push_back(100 + pas);}
    void release() {++freed; events.push_back(200 + pas);}
};
struct Image {
    void *file = nullptr;
    Buffer *meta = nullptr;
    bool prepared = false, secureMetadata = false;
    uint32_t pas = 0;
    bool pasActive = false;
};
static Image gAdspImage, gDtbImage;
static bool gProxyHeld, gLoadStateOn;
static unsigned failedPas;
static int voteResult, qmpResult;
struct ScmResult {int64_t x0; uint64_t x1;};
constexpr unsigned QCOM_SCM_PAS_SHUTDOWN = 6;
static unsigned qcom_scm_args(unsigned, unsigned, unsigned, unsigned) {return 1;}
static ScmResult scm(unsigned operation, unsigned, unsigned pas, unsigned, unsigned) {
    CHECK(operation == QCOM_SCM_PAS_SHUTDOWN);
    events.push_back(pas);
    return {pas == failedPas ? 2 : 0, pas == failedPas ? 1u : 0u};
}
static bool scmOk(const ScmResult &result) {return !result.x0 && !result.x1;}
static void sheng_fw_close(void *) {events.push_back(300);}
static int vote(unsigned, unsigned) {events.push_back(400); if (!voteResult) gProxyHeld = false; return voteResult;}
static int sm7675_remote_qmp(const char *) {events.push_back(500); return qmpResult;}
// PRODUCTION_METADATA_LIFECYCLE

static void reset() {
    events.clear(); gAdspImage = {}; gDtbImage = {};
    gProxyHeld = gLoadStateOn = true;
    failedPas = 0; voteResult = qmpResult = 0;
}
int main() {
    // Failed INIT_IMAGE still exposed the metadata to secure firmware.
    reset(); Buffer dtb = {36};
    gDtbImage = {(void *)1, &dtb, true, true, 36};
    release(gDtbImage);
    CHECK(gDtbImage.meta == &dtb && !dtb.freed && !dtb.completed && !gDtbImage.file);
    unwindStart();
    CHECK(!gDtbImage.meta && dtb.freed == 1 && dtb.completed == 1);
    CHECK((events == std::vector<unsigned>{300, 36, 136, 236, 400, 500}));
    CHECK(!gProxyHeld && !gLoadStateOn);

    // A refused shutdown cannot return a potentially protected page to the VM.
    reset(); dtb = {36}; failedPas = 36;
    gDtbImage = {nullptr, &dtb, true, true, 36};
    unwindStart();
    CHECK(gDtbImage.secureMetadata && gDtbImage.meta == &dtb);
    CHECK(!dtb.freed && !dtb.completed && gProxyHeld && gLoadStateOn);
    release(gDtbImage);
    CHECK(!dtb.freed && !dtb.completed && gDtbImage.meta == &dtb);
    CHECK((events == std::vector<unsigned>{36}));

    // Later ADSP failure unwinds both PAS instances before either is freed.
    reset(); Buffer adsp = {1}; dtb = {36};
    gAdspImage = {nullptr, &adsp, true, true, 1};
    gDtbImage = {nullptr, &dtb, true, true, 36};
    unwindStart();
    CHECK((events == std::vector<unsigned>{1, 36, 101, 201, 136, 236, 400, 500}));
    CHECK(adsp.freed == 1 && dtb.freed == 1 && !gAdspImage.secureMetadata && !gDtbImage.secureMetadata);

    // Refusing the first shutdown also keeps the other image and all votes.
    reset(); adsp = {1}; dtb = {36}; failedPas = 1;
    gAdspImage = {nullptr, &adsp, true, true, 1};
    gDtbImage = {nullptr, &dtb, true, true, 36};
    unwindStart();
    CHECK((events == std::vector<unsigned>{1}));
    CHECK(!adsp.freed && !dtb.freed && gProxyHeld && gLoadStateOn);

    // Before secure exposure, ordinary parse/allocation failures release freely.
    reset(); adsp = {1}; gAdspImage = {(void *)1, &adsp, true, false, 0};
    release(gAdspImage);
    CHECK(adsp.freed == 1 && adsp.completed == 1 && !gAdspImage.meta);
    CHECK((events == std::vector<unsigned>{300, 101, 201}));
    release(gAdspImage);
    CHECK(adsp.freed == 1 && adsp.completed == 1);

    // Successful authentication/start completion permits metadata release.
    reset(); adsp = {1}; dtb = {36};
    gAdspImage = {nullptr, &adsp, true, false, 1};
    gDtbImage = {nullptr, &dtb, true, false, 36};
    release(gDtbImage); release(gAdspImage);
    CHECK((events == std::vector<unsigned>{136, 236, 101, 201}));
    CHECK(gProxyHeld && gLoadStateOn);
    // A later GLINK setup failure must stop live firmware even after its
    // transient metadata buffers were released on successful start.
    events.clear();
    gAdspImage.pasActive = gDtbImage.pasActive = true;
    unwindStart();
    CHECK((events == std::vector<unsigned>{1, 36, 400, 500}));
    CHECK(!gAdspImage.pasActive && !gDtbImage.pasActive && !gProxyHeld && !gLoadStateOn);
    CHECK(adsp.freed == 1 && dtb.freed == 1);
    printf("PASS production ADSP shutdown-before-metadata-release: %u checks\n", checks);
}
