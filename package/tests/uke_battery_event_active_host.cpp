/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Production inbox factory plus actual XNU classifier, with the observed ABI. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static unsigned checks, allocations, releases, callbacks;
static bool failAllocation, failInit;
#define CHECK(x) do {++checks; assert(x);} while (0)
#define OSDeclareDefaultStructors(name)
#define OSDefineMetaClassAndStructors(name, parent)
struct OSObject {};
struct IOEventSource {
    enum {kPassive = 1, kActive = 2, kActionBlock = 4};
    uint16_t flags = 0;
    virtual ~IOEventSource() = default;
    virtual bool checkForWork() {return false;}
};
struct IOInterruptEventSource : IOEventSource {
    using Action = void (*)(OSObject *, IOInterruptEventSource *, int);
    OSObject *owner = nullptr;
    Action action = nullptr;
    unsigned producer = 0, consumer = 0;
    bool init(OSObject *o, Action a) {owner = o; action = a; return !failInit;}
    void release() {++releases; delete this;}
    void interruptOccurred(void *, void *, int) {++producer;}
    bool checkForWork() override {
        if (producer != consumer) {const unsigned n = producer-consumer; consumer=producer; action(owner,this,n);}
        return false;
    }
};
template <class T> static T *allocate() {++allocations; return failAllocation ? nullptr : new T;}
#define OSTypeAlloc(name) allocate<name>()
// PRODUCTION_ACTIVE_INBOX
struct IOWorkLoop {
    IOEventSource control;
    IOEventSource *controlG = &control;
    bool eventSourcePerformsWork(IOEventSource *);
};
static void virtualCheckThunk() {}
// Actual final _ptmf2ptf returns the signed thunk without using 'self'.
#define OSMemberFunctionCast(type, object, member) ((void)(object), reinterpret_cast<type>(&virtualCheckThunk))
// PRODUCTION_CLASSIFIER
static void action(OSObject *, IOInterruptEventSource *, int n) {callbacks += n;}
int main() {
    IOWorkLoop loop; OSObject owner;
    // Same source init/checkForWork as before: notifications arrive but the
    // thunk comparison excludes the source from the work loop's active chain.
    IOInterruptEventSource old;
    CHECK(old.init(&owner, action)); old.interruptOccurred(nullptr,nullptr,0);
    CHECK(!loop.eventSourcePerformsWork(&old));
    if (loop.eventSourcePerformsWork(&old)) old.checkForWork();
    CHECK(callbacks == 0 && old.producer == 1 && old.consumer == 0);
    auto *inbox = UkeBatteryInbox::create(&owner, action);
    CHECK(inbox && inbox->flags == IOEventSource::kActive);
    CHECK(loop.eventSourcePerformsWork(inbox));
    for (unsigned i=0; i<53; ++i) inbox->interruptOccurred(nullptr,nullptr,0);
    CHECK(callbacks == 0);
    if (loop.eventSourcePerformsWork(inbox)) inbox->checkForWork();
    CHECK(callbacks == 53 && inbox->producer == inbox->consumer);
    inbox->checkForWork(); CHECK(callbacks == 53);
    CHECK(!loop.eventSourcePerformsWork(&loop.control)); // Command gates stay passive.
    inbox->release(); CHECK(releases == 1);
    failInit = true; CHECK(!UkeBatteryInbox::create(&owner, action)); CHECK(releases == 2);
    failInit = false; failAllocation = true;
    CHECK(!UkeBatteryInbox::create(&owner, action)); CHECK(releases == 2 && allocations == 3);
    printf("PASS actual XNU active-source classifier and production battery inbox: %u checks\n", checks);
}
