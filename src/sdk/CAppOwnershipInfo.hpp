#pragma once

#include <cstddef>
#include <cstdint>

struct CAppOwnershipInfo {
    int32_t subId;
    int32_t releaseState;
    uint32_t owner;
    int32_t masterSubscriptionAppId;
    uint32_t trialTime;
    int32_t existInPackageNums;
    char region[2];
    char field7_0x1A[2];
    uint32_t purchaseTime;
    uint32_t realOwner;
    bool ownsLicense;
    bool licenseExpired;
    bool field12_0x26;
    bool lowViolence;
    bool freeLicense;
    bool regionRestricted;
    bool fromFreeWeekend;
    bool licenseLocked;
    bool licensePending;
    bool retailLicense;
    bool autoGrant;
    bool licensePermanent;
    bool field21_0x30;
    bool field22_0x31;
    bool siteLicense;
    bool field24_0x33;
    bool field25_0x34;
    bool familyShared;
    bool field27_0x36;
    bool field28_0x37;
}; //0x38

static_assert(offsetof(CAppOwnershipInfo, existInPackageNums) == 0x14, "CAppOwnershipInfo::existInPackageNums offset mismatch");
static_assert(sizeof(CAppOwnershipInfo) == 0x38, "CAppOwnershipInfo size mismatch");
