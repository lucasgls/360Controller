/*
 * XboxControllerDriver.cpp
 * Implementation — Xbox 360 Wireless Controller HID (DriverKit / HIDDriverKit)
 *
 * In DriverKit, override the _Impl methods that the IIG compiler generates:
 *   Start_Impl / Stop_Impl / newDeviceDescription_Impl /
 *   newReportDescriptor_Impl / handleReport_Impl
 *
 * GPL v2 — see Licence.txt
 */

#include <DriverKit/OSData.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSString.h>
#include <os/log.h>
// IIG-generated header — found via HEADER_SEARCH_PATHS = $(DERIVED_FILE_DIR)
#include "XboxControllerDriver.h"

#define LOG(fmt, ...)                                                          \
  os_log(OS_LOG_DEFAULT, "[XboxController] " fmt, ##__VA_ARGS__)

// ─── HID Report Descriptor — Xbox 360 gamepad layout ─────────────────────────
// Botões ordenados conforme a sequência real dos bits no pacote USB do Xbox 360
static const uint8_t kReportDescriptor[] = {
    0x05, 0x01, // USAGE_PAGE (Generic Desktop)
    0x09, 0x05, // USAGE (Game Pad)
    0xa1, 0x01, // COLLECTION (Application)
    0x05, 0x01, //   USAGE_PAGE (Generic Desktop)
    0x09, 0x3a, //   USAGE (Counted Buffer)
    0xa1, 0x02, //   COLLECTION (Logical)
    0x75, 0x08, //     REPORT_SIZE (8)
    0x95, 0x02, //     REPORT_COUNT (2)
    0x05, 0x01, //     USAGE_PAGE (Generic Desktop)
    0x09, 0x3f, //     USAGE (Reserved)
    0x09, 0x3b, //     USAGE (Byte Count)
    0x81, 0x01, //     INPUT (Cnst,Ary,Abs)

    // ── Grupo 1: D-pad — Up, Down, Left, Right (botões 1–4) ──────────────────
    0x75, 0x01, //     REPORT_SIZE (1)
    0x15, 0x00, //     LOGICAL_MINIMUM (0)
    0x25, 0x01, //     LOGICAL_MAXIMUM (1)
    0x35, 0x00, //     PHYSICAL_MINIMUM (0)
    0x45, 0x01, //     PHYSICAL_MAXIMUM (1)
    0x95, 0x04, //     REPORT_COUNT (4)
    0x05, 0x09, //     USAGE_PAGE (Button)
    0x19, 0x01, //     USAGE_MINIMUM (Button 1)
    0x29, 0x04, //     USAGE_MAXIMUM (Button 4)
    0x81, 0x02, //     INPUT (Data,Var,Abs)

    // ── Grupo 2: Start, Back, LS, RS (botões 5–8) ────────────────────────────
    0x75, 0x01, //     REPORT_SIZE (1)
    0x15, 0x00, //     LOGICAL_MINIMUM (0)
    0x25, 0x01, //     LOGICAL_MAXIMUM (1)
    0x35, 0x00, //     PHYSICAL_MINIMUM (0)
    0x45, 0x01, //     PHYSICAL_MAXIMUM (1)
    0x95, 0x04, //     REPORT_COUNT (4)
    0x05, 0x09, //     USAGE_PAGE (Button)
    0x09, 0x05, //     USAGE (Button 5)  ← Start
    0x09, 0x06, //     USAGE (Button 6)  ← Back
    0x09, 0x07, //     USAGE (Button 7)  ← LS
    0x09, 0x08, //     USAGE (Button 8)  ← RS
    0x81, 0x02, //     INPUT (Data,Var,Abs)

    // ── Grupo 3: LB, RB, Guide (botões 9–11) + 1 bit padding ─────────────────
    0x75, 0x01, //     REPORT_SIZE (1)
    0x15, 0x00, //     LOGICAL_MINIMUM (0)
    0x25, 0x01, //     LOGICAL_MAXIMUM (1)
    0x35, 0x00, //     PHYSICAL_MINIMUM (0)
    0x45, 0x01, //     PHYSICAL_MAXIMUM (1)
    0x95, 0x03, //     REPORT_COUNT (3)
    0x05, 0x09, //     USAGE_PAGE (Button)
    0x09, 0x09, //     USAGE (Button 9)   ← LB
    0x09, 0x0a, //     USAGE (Button 10)  ← RB
    0x09, 0x0b, //     USAGE (Button 11)  ← Guide
    0x81, 0x02, //     INPUT (Data,Var,Abs)
    0x75, 0x01, //     REPORT_SIZE (1)
    0x95, 0x01, //     REPORT_COUNT (1)
    0x81, 0x01, //     INPUT (Cnst,Ary,Abs) — padding bit

    // ── Grupo 4: A, B, X, Y (botões 12–15) ───────────────────────────────────
    0x75, 0x01, //     REPORT_SIZE (1)
    0x15, 0x00, //     LOGICAL_MINIMUM (0)
    0x25, 0x01, //     LOGICAL_MAXIMUM (1)
    0x35, 0x00, //     PHYSICAL_MINIMUM (0)
    0x45, 0x01, //     PHYSICAL_MAXIMUM (1)
    0x95, 0x04, //     REPORT_COUNT (4)
    0x05, 0x09, //     USAGE_PAGE (Button)
    0x19, 0x0c, //     USAGE_MINIMUM (Button 12) ← A
    0x29, 0x0f, //     USAGE_MAXIMUM (Button 15) ← Y
    0x81, 0x02, //     INPUT (Data,Var,Abs)

    // ── Gatilhos: LT (Z) e RT (Rz) — 8 bits cada ─────────────────────────────
    0x75, 0x08,       //     REPORT_SIZE (8)
    0x15, 0x00,       //     LOGICAL_MINIMUM (0)
    0x26, 0xff, 0x00, //     LOGICAL_MAXIMUM (255)
    0x35, 0x00,       //     PHYSICAL_MINIMUM (0)
    0x46, 0xff, 0x00, //     PHYSICAL_MAXIMUM (255)
    0x95, 0x02,       //     REPORT_COUNT (2)
    0x05, 0x01,       //     USAGE_PAGE (Generic Desktop)
    0x09, 0x32,       //     USAGE (Z)  ← LT
    0x09, 0x35,       //     USAGE (Rz) ← RT
    0x81, 0x02,       //     INPUT (Data,Var,Abs)

    // ── Analógico esquerdo: X, Y — 16 bits cada
    // ───────────────────────────────
    0x75, 0x10,       //     REPORT_SIZE (16)
    0x16, 0x00, 0x80, //     LOGICAL_MINIMUM (-32768)
    0x26, 0xff, 0x7f, //     LOGICAL_MAXIMUM (32767)
    0x36, 0x00, 0x80, //     PHYSICAL_MINIMUM (-32768)
    0x46, 0xff, 0x7f, //     PHYSICAL_MAXIMUM (32767)
    0x05, 0x01,       //     USAGE_PAGE (Generic Desktop)
    0x09, 0x01,       //     USAGE (Pointer)
    0xa1, 0x00,       //     COLLECTION (Physical)
    0x95, 0x02,       //       REPORT_COUNT (2)
    0x05, 0x01,       //       USAGE_PAGE (Generic Desktop)
    0x09, 0x30,       //       USAGE (X)
    0x09, 0x31,       //       USAGE (Y)
    0x81, 0x02,       //       INPUT (Data,Var,Abs)
    0xc0,             //     END_COLLECTION

    // ── Analógico direito: Rx, Ry — 16 bits cada
    // ──────────────────────────────
    0x05, 0x01, //     USAGE_PAGE (Generic Desktop)
    0x09, 0x01, //     USAGE (Pointer)
    0xa1, 0x00, //     COLLECTION (Physical)
    0x95, 0x02, //       REPORT_COUNT (2)
    0x05, 0x01, //       USAGE_PAGE (Generic Desktop)
    0x09, 0x33, //       USAGE (Rx)
    0x09, 0x34, //       USAGE (Ry)
    0x81, 0x02, //       INPUT (Data,Var,Abs)
    0xc0,       //     END_COLLECTION

    0xc0, //   END_COLLECTION
    0xc0  // END_COLLECTION
};

// ─────────────────────────────────────────────────────────────────────────────
// Start / Stop
// ─────────────────────────────────────────────────────────────────────────────

kern_return_t XboxControllerDriver::Start_Impl(IOService_Start_Args) {
  kern_return_t ret = Start(provider, SUPERDISPATCH);
  if (ret != kIOReturnSuccess)
    return ret;

  LOG("Start — Xbox 360 Wireless Controller HID driver");
  RegisterService();
  return kIOReturnSuccess;
}

kern_return_t XboxControllerDriver::Stop_Impl(IOService_Stop_Args) {
  LOG("Stop — Xbox 360 Wireless Controller HID driver");
  return Stop(provider, SUPERDISPATCH);
}

// ─────────────────────────────────────────────────────────────────────────────
// HIDDriverKit _Impl overrides
// ─────────────────────────────────────────────────────────────────────────────

OSDictionary *XboxControllerDriver::newDeviceDescription() {
  OSDictionary *dict = OSDictionary::withCapacity(8);
  if (!dict)
    return nullptr;

  dict->setObject("VendorID", OSNumber::withNumber((uint64_t)0x045E, 32));
  dict->setObject("ProductID", OSNumber::withNumber((uint64_t)0x028E, 32));
  dict->setObject("VersionNumber", OSNumber::withNumber((uint64_t)0x0114, 32));
  // This driver is an internal helper for the receiver stack.
  // We intentionally do NOT advertise as a standard gamepad to avoid
  // duplicate devices in GameController/Web Gamepad API.
  dict->setObject("PrimaryUsagePage",
                  OSNumber::withNumber((uint64_t)0xFF00, 32)); // Vendor-defined
  dict->setObject("PrimaryUsage", OSNumber::withNumber((uint64_t)0x01, 32));
  dict->setObject("Manufacturer", OSString::withCString("Microsoft"));
  dict->setObject("Product",
                  OSString::withCString("Wireless 360 Controller (internal)"));
  dict->setObject("Transport", OSString::withCString("Wireless"));

  return dict;
}

OSData *XboxControllerDriver::newReportDescriptor() {
  return OSData::withBytes(kReportDescriptor, sizeof(kReportDescriptor));
}

kern_return_t XboxControllerDriver::handleReport(uint64_t timestamp,
                                                 IOMemoryDescriptor *report,
                                                 uint32_t reportLength,
                                                 IOHIDReportType reportType,
                                                 IOOptionBits options) {
  // HID reports are submitted here by XboxReceiverDriver.
  // Return success — HIDDriverKit handles event dispatch internally.
  return kIOReturnSuccess;
}