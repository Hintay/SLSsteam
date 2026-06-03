#pragma once

#include "../sdk/PackageInfo.hpp"

#include <cstdint>

namespace Package {
    // True once tryInitFakeLicenseOnce has injected into pkg0 successfully.
    bool isActive();

    // Append appId into vec's spare capacity only (no realloc). Returns false if
    // the vector is full (caller logs + drops; first version never reallocs — see
    // plan note: observed SPARE=10 covers typical lua sets).
    // Caller must hold the package mutex (no internal locking).
    bool appendAppIdInPlace(CUtlVector<uint32_t>* vec, uint32_t appId);

    // Linear find + overwrite-with-last + m_Size--. Returns false if absent.
    // Caller must hold the package mutex (no internal locking).
    bool findAndFastRemove(CUtlVector<uint32_t>* vec, uint32_t appId);

    // Capture seam (set by hooks.cpp). pkg0 PackageInfo*, CUser*.
    void setInjectedPackage(void* pkg);
    void setCUser(void* cuser);

    // Spec A onDepotsChanged callback target (registered in Hooks::setup).
    void notifyLicenseChanged();

    // One-shot: inject getAllDepotIds() into pkg0 + Mark/Process. Safe to call
    // repeatedly; only runs once when pkg0+CUser captured and Status==Available.
    void tryInitFakeLicenseOnce();
}
