/*
 * XboxReceiverDriver.cpp
 * Xbox 360 Wireless Gaming Receiver — DriverKit / USBDriverKit 25.4
 *
 * Ported from WirelessGamingReceiver.cpp (Colin Munro, 2006-2013)
 * GPL v2 — see Licence.txt
 *
 * Publishes itself as IOUserHIDDevice so the controller appears as a
 * standard HID gamepad to macOS, the GameController framework, and
 * web Gamepad API testers.
 */

#include "XboxReceiverDriver.h"
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/OSAction.h>
#include <DriverKit/OSData.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSString.h>
#include <HIDDriverKit/IOUserHIDDevice.h>
#include <USBDriverKit/IOUSBHostDevice.h>
#include <USBDriverKit/IOUSBHostInterface.h>
#include <USBDriverKit/IOUSBHostPipe.h>
#include <os/log.h>
#include <string.h>

#define LOG(fmt, ...)                                                          \
  os_log(OS_LOG_DEFAULT, "[XboxReceiver] " fmt, ##__VA_ARGS__)

static const uint32_t kReadBufSize = 32;
static const uint32_t kWriteBufSize = 12;

// ── HID report descriptor — Standard gamepad layout ──────────────────────────
// Keep the HID descriptor and the bit packing in sync. We expose:
// - 16 digital buttons in the common Web/GameController "standard" order
// - 2 analog triggers (Z/Rz, 0-255) placed *after* the 4 stick axes
// - 2 sticks (X/Y and Rx/Ry, int16) as the first 4 axes
//
// Input report (13 bytes total):
//   Byte 0:     Report ID (1)
//   Byte 1-2:   Buttons (16 bits, per descriptor order below)
//   Byte 3:     Left Trigger  (0-255)
//   Byte 4:     Right Trigger (0-255)
//   Byte 5-6:   Left Stick X  (int16)
//   Byte 7-8:   Left Stick Y  (int16)  (we invert in software for "up is up")
//   Byte 9-10:  Right Stick X (int16)
//   Byte 11-12: Right Stick Y (int16) (we invert in software for "up is up")

static const uint8_t kHIDReportDescriptor[] = {
    0x05, 0x01, // USAGE_PAGE (Generic Desktop)
    0x09, 0x05, // USAGE (Game Pad)
    0xa1, 0x01, // COLLECTION (Application)
    0x85, 0x01, //   REPORT_ID (1)

    // ── 16 buttons (standard order via packing below) ──────────────────────
    0x05, 0x09, //   USAGE_PAGE (Button)
    0x19, 0x01, //   USAGE_MINIMUM (Button 1)
    0x29, 0x10, //   USAGE_MAXIMUM (Button 16)
    0x15, 0x00, //   LOGICAL_MINIMUM (0)
    0x25, 0x01, //   LOGICAL_MAXIMUM (1)
    0x95, 0x10, //   REPORT_COUNT (16)
    0x75, 0x01, //   REPORT_SIZE (1)
    0x81, 0x02, //   INPUT (Data,Var,Abs)

    // ── Triggers first: Z, Rz (0-255) — matches struct layout ─────────────
    0x75, 0x08,       //   REPORT_SIZE (8)
    0x15, 0x00,       //   LOGICAL_MINIMUM (0)
    0x26, 0xff, 0x00, //   LOGICAL_MAXIMUM (255)
    0x95, 0x02,       //   REPORT_COUNT (2)
    0x05, 0x01,       //   USAGE_PAGE (Generic Desktop)
    0x09, 0x32,       //   USAGE (Z)  ← LT
    0x09, 0x35,       //   USAGE (Rz) ← RT
    0x81, 0x02,       //   INPUT (Data,Var,Abs)

    // ── 4 stick axes: X, Y, Rx, Ry (int16) ─────────────────────────────────
    0x75, 0x10,       //   REPORT_SIZE (16)
    0x16, 0x00, 0x80, //   LOGICAL_MINIMUM (-32768)
    0x26, 0xff, 0x7f, //   LOGICAL_MAXIMUM (32767)
    0x05, 0x01,       //   USAGE_PAGE (Generic Desktop)
    0x95, 0x04,       //   REPORT_COUNT (4)
    0x09, 0x30,       //   USAGE (X)
    0x09, 0x31,       //   USAGE (Y)
    0x09, 0x33,       //   USAGE (Rx)
    0x09, 0x34,       //   USAGE (Ry)
    0x81, 0x02,       //   INPUT (Data,Var,Abs)

    0xc0 // END_COLLECTION
};

// ── HID report buffer ────────────────────────────────────────────────────────
#pragma pack(push, 1)
struct XboxHIDReport {
  uint8_t reportId; // 1
  uint16_t buttons; // bitfield
  uint8_t triggerL;
  uint8_t triggerR;
  int16_t leftX;
  int16_t leftY;
  int16_t rightX;
  int16_t rightY;
};
#pragma pack(pop)
static_assert(sizeof(XboxHIDReport) == 13, "HID report must be 13 bytes");

// ── Driver state
// ──────────────────────────────────────────────────────────────

static IOUSBHostDevice *s_dev = nullptr;
static IOUSBHostInterface *s_intf = nullptr;
static IOUSBHostPipe *s_inPipe = nullptr;
static IOUSBHostPipe *s_outPipe = nullptr;
static IOBufferMemoryDescriptor *s_readBuf = nullptr;
static IOBufferMemoryDescriptor *s_writeBuf = nullptr;
static IOBufferMemoryDescriptor *s_hidBuf = nullptr;
static OSAction *s_readAction = nullptr;
static OSAction *s_writeAction = nullptr;
static bool s_connected = false;
static bool s_ready = false;
static bool s_ledSent = false;

// ─────────────────────────────────────────────────────────────────────────────
// IOUserHIDDevice overrides — newDeviceDescription / newReportDescriptor
// ─────────────────────────────────────────────────────────────────────────────

OSDictionary *XboxReceiverDriver::newDeviceDescription(void) {
  OSDictionary *dict = OSDictionary::withCapacity(8);
  if (!dict)
    return nullptr;

  // Transport
  OSString *transport = OSString::withCString("USB");
  if (transport) {
    dict->setObject("Transport", transport);
    transport->release();
  }

  // VendorID = Microsoft (0x045E = 1118)
  OSNumber *vid = OSNumber::withNumber(0x045E, 32);
  if (vid) {
    dict->setObject("VendorID", vid);
    vid->release();
  }

  // ProductID = Xbox 360 Wireless Controller (0x028E = 654)
  OSNumber *pid = OSNumber::withNumber(0x028E, 32);
  if (pid) {
    dict->setObject("ProductID", pid);
    pid->release();
  }

  // Product name
  OSString *product = OSString::withCString("Xbox 360 Wireless Controller");
  if (product) {
    dict->setObject("Product", product);
    product->release();
  }

  // Manufacturer
  OSString *mfg = OSString::withCString("Microsoft");
  if (mfg) {
    dict->setObject("Manufacturer", mfg);
    mfg->release();
  }

  // PrimaryUsagePage = Generic Desktop (0x01), PrimaryUsage = Gamepad (0x05)
  // Required for macOS/browsers to classify this as a gamepad
  OSNumber *usagePage = OSNumber::withNumber(0x01, 32);
  if (usagePage) {
    dict->setObject("PrimaryUsagePage", usagePage);
    usagePage->release();
  }
  OSNumber *usage = OSNumber::withNumber(0x05, 32);
  if (usage) {
    dict->setObject("PrimaryUsage", usage);
    usage->release();
  }

  LOG("newDeviceDescription created");
  return dict;
}

OSData *XboxReceiverDriver::newReportDescriptor(void) {
  LOG("newReportDescriptor — %zu bytes", sizeof(kHIDReportDescriptor));
  return OSData::withBytes(kHIDReportDescriptor, sizeof(kHIDReportDescriptor));
}

// ─────────────────────────────────────────────────────────────────────────────
// Start_Impl
// ─────────────────────────────────────────────────────────────────────────────

kern_return_t XboxReceiverDriver::Start_Impl(IOService_Start_Args) {
  kern_return_t ret;
  LOG("Start — Xbox 360 Wireless Gaming Receiver (HID)");

  ret = Start(provider, SUPERDISPATCH);
  if (ret != kIOReturnSuccess) {
    LOG("super::Start failed 0x%x", ret);
    return ret;
  }

  // ── Allocate HID report buffer ───────────────────────────────────────────
  IOBufferMemoryDescriptor *hidBuf = nullptr;
  ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOut,
                                         sizeof(XboxHIDReport), 0, &hidBuf);
  if (ret != kIOReturnSuccess || !hidBuf) {
    LOG("HID buffer alloc failed");
    Stop(provider, SUPERDISPATCH);
    return ret;
  }
  s_hidBuf = hidBuf;

  // ── 1. Open USB device ────────────────────────────────────────────────────
  IOUSBHostDevice *dev = OSDynamicCast(IOUSBHostDevice, provider);
  if (!dev) {
    LOG("Provider is not IOUSBHostDevice");
    Stop(provider, SUPERDISPATCH);
    return kIOReturnNoDevice;
  }

  ret = dev->Open(this, 0, 0);
  if (ret != kIOReturnSuccess) {
    LOG("Device Open failed 0x%x", ret);
    Stop(provider, SUPERDISPATCH);
    return ret;
  }
  s_dev = dev;
  LOG("Device opened");

  // ── 1b. SetConfiguration — creates interface children ────────────────────
  ret = dev->SetConfiguration(1, true);
  if (ret == kIOReturnSuccess) {
    LOG("SetConfiguration(1) OK");
  } else {
    LOG("SetConfiguration(1) failed 0x%x", ret);
  }

  // ── 2. Iterate interfaces using CreateInterfaceIterator ──────────────────
  uintptr_t iterRef = 0;
  ret = dev->CreateInterfaceIterator(&iterRef);
  if (ret != kIOReturnSuccess) {
    LOG("CreateInterfaceIterator failed 0x%x", ret);
    RegisterService();
    return kIOReturnSuccess;
  }
  LOG("Interface iterator created");

  IOUSBHostInterface *intf = nullptr;
  IOUSBHostInterface *firstIntf = nullptr;
  int ifaceCount = 0;

  while (dev->CopyInterface(iterRef, &intf) == kIOReturnSuccess && intf) {
    LOG("Found interface %d", ifaceCount);
    if (ifaceCount == 0) {
      firstIntf = intf;
    } else {
      intf->release();
    }
    intf = nullptr;
    ifaceCount++;
  }
  dev->DestroyInterfaceIterator(iterRef);
  LOG("Found %d interface(s)", ifaceCount);

  if (!firstIntf) {
    LOG("No interfaces found");
    RegisterService();
    return kIOReturnSuccess;
  }

  // ── 3. Open first interface (controller slot 0) ──────────────────────────
  ret = firstIntf->Open(this, 0, nullptr);
  if (ret != kIOReturnSuccess) {
    LOG("Interface Open failed 0x%x", ret);
    firstIntf->release();
    RegisterService();
    return kIOReturnSuccess;
  }
  s_intf = firstIntf;
  LOG("Interface 0 opened");

  // ── 4. IN pipe — receive controller data ─────────────────────────────────
  IOUSBHostPipe *inPipe = nullptr;
  ret = firstIntf->CopyPipe(0x81, &inPipe);
  if (ret != kIOReturnSuccess || !inPipe) {
    for (uint8_t a = 0x82; a <= 0x88 && !inPipe; a++) {
      ret = firstIntf->CopyPipe(a, &inPipe);
    }
  }
  if (!inPipe) {
    LOG("No IN pipe found");
    RegisterService();
    return kIOReturnSuccess;
  }
  s_inPipe = inPipe;
  LOG("IN pipe found");

  IOBufferMemoryDescriptor *readBuf = nullptr;
  ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionIn, kReadBufSize, 0,
                                         &readBuf);
  if (ret != kIOReturnSuccess || !readBuf) {
    LOG("Read buffer alloc failed");
    RegisterService();
    return kIOReturnSuccess;
  }
  s_readBuf = readBuf;

  OSAction *readAction = nullptr;
  ret = CreateActionReadComplete(0, &readAction);
  if (ret != kIOReturnSuccess || !readAction) {
    LOG("CreateActionReadComplete failed");
    RegisterService();
    return kIOReturnSuccess;
  }
  s_readAction = readAction;

  ret = inPipe->AsyncIO(readBuf, kReadBufSize, readAction, 0);
  if (ret == kIOReturnSuccess) {
    LOG("Async read queued — listening for controller data");
  } else {
    LOG("AsyncIO(IN) failed 0x%x", ret);
  }

  // ── 5. OUT pipe — LED/rumble commands ────────────────────────────────────
  IOUSBHostPipe *outPipe = nullptr;
  ret = firstIntf->CopyPipe(0x01, &outPipe);
  if (ret != kIOReturnSuccess || !outPipe) {
    for (uint8_t a = 0x02; a <= 0x08 && !outPipe; a++) {
      ret = firstIntf->CopyPipe(a, &outPipe);
    }
  }
  if (outPipe) {
    s_outPipe = outPipe;
    IOBufferMemoryDescriptor *wb = nullptr;
    IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOut, kWriteBufSize, 0,
                                     &wb);
    if (wb) {
      s_writeBuf = wb;
      OSAction *wa = nullptr;
      CreateActionWriteComplete(0, &wa);
      if (wa)
        s_writeAction = wa;
    }
    LOG("OUT pipe ready");
  } else {
    LOG("No OUT pipe");
  }

  LOG("Driver ready — press Xbox button on controller to pair");
  RegisterService();
  return kIOReturnSuccess;
}

// ─────────────────────────────────────────────────────────────────────────────
// Stop_Impl
// ─────────────────────────────────────────────────────────────────────────────

kern_return_t XboxReceiverDriver::Stop_Impl(IOService_Stop_Args) {
  LOG("Stop");
  if (s_inPipe)
    s_inPipe->Abort(0, kIOReturnAborted, this);
  if (s_outPipe)
    s_outPipe->Abort(0, kIOReturnAborted, this);
  OSSafeReleaseNULL(s_writeAction);
  OSSafeReleaseNULL(s_readAction);
  OSSafeReleaseNULL(s_writeBuf);
  OSSafeReleaseNULL(s_readBuf);
  OSSafeReleaseNULL(s_hidBuf);
  OSSafeReleaseNULL(s_outPipe);
  OSSafeReleaseNULL(s_inPipe);
  if (s_intf) {
    s_intf->Close(this, 0);
    s_intf->release();
    s_intf = nullptr;
  }
  s_dev = nullptr;
  s_connected = false;
  s_ready = false;
  s_ledSent = false;
  return Stop(provider, SUPERDISPATCH);
}

// ─────────────────────────────────────────────────────────────────────────────
// ReadComplete_Impl — Parse wireless protocol + dispatch HID report
// ─────────────────────────────────────────────────────────────────────────────

void XboxReceiverDriver::ReadComplete_Impl(
    XboxReceiverDriver_ReadComplete_Args) {

  if (status == kIOReturnAborted) {
    LOG("ReadComplete: aborted");
    return;
  }

  // Clear any pipe stall before we attempt to re-queue below.
  // kIOReturnNotResponding / kIOReturnOverrun / stall codes all leave the
  // endpoint halted; ClearStall(false) resets the data-toggle and lets
  // the next AsyncIO succeed.  Without this the re-queue always returns
  // 0xe0005000 and the pipe stays dead forever.
  if (status != kIOReturnSuccess && status != kIOReturnOverrun) {
    LOG("ReadComplete error 0x%x — clearing pipe stall", status);
    if (s_inPipe)
      s_inPipe->ClearStall(false);
    goto requeue;
  }

  if ((status == kIOReturnSuccess || status == kIOReturnOverrun) &&
      actualByteCount > 0 && s_readBuf) {
    IOAddressSegment range = {};
    if (s_readBuf->GetAddressRange(&range) == kIOReturnSuccess &&
        range.address) {
      const uint8_t *d = reinterpret_cast<const uint8_t *>(range.address);
      uint32_t len = actualByteCount;

      // ── Connection event: len==2, d[0]==0x08 ─────────────────────────
      if (len == 2 && d[0] == 0x08) {
        if (d[1] == 0x00) {
          LOG("Controller DISCONNECTED");
          s_connected = false;
          s_ready = false;
          s_ledSent = false;
        } else {
          LOG("Controller CONNECTED");
          s_connected = true;
          s_ledSent = false;
        }
      }

      // ── Send LED command as soon as controller is connected ──────────
      if (s_connected && !s_ledSent && s_outPipe && s_writeBuf &&
          s_writeAction) {
        s_ledSent = true;
        LOG("Sending LED command");

        IOAddressSegment wrange = {};
        if (s_writeBuf->GetAddressRange(&wrange) == kIOReturnSuccess &&
            wrange.address) {
          uint8_t *w = reinterpret_cast<uint8_t *>(wrange.address);
          memset(w, 0, kWriteBufSize);
          w[0] = 0x00; // slot 0
          w[1] = 0x00;
          w[2] = 0x08; // LED command
          w[3] = 0x42; // player 1 solid

          kern_return_t wr =
              s_outPipe->AsyncIO(s_writeBuf, kWriteBufSize, s_writeAction, 0);
          if (wr == kIOReturnSuccess) {
            LOG("LED command sent");
          } else {
            LOG("LED command failed 0x%x", wr);
            s_ledSent = false;
          }
        }
      }

      // ── Serial number exchange ───────────────────────────────────────
      if (len > 1 && d[1] == 0x0f && s_connected && !s_ready) {
        s_ready = true;
        LOG("Controller READY");
      }

      // ── Input report → HID ──────────────────────────────────────────
      // Packet: d[0]=slot d[1]=0x01 d[2]=0x00 d[3]=0xf0
      //         d[4]=reportId d[5]=length d[6..7]=buttons
      //         d[8]=LT d[9]=RT d[10..17]=sticks
      if (len >= 18 && d[0] == 0x00 && d[1] == 0x01 && d[3] == 0xf0) {
        if (!s_connected)
          s_connected = true;

        uint16_t xboxBtns = (uint16_t)d[6] | ((uint16_t)d[7] << 8);
        uint8_t lt = d[8];
        uint8_t rt = d[9];
        int16_t lx = (int16_t)((uint16_t)d[10] | ((uint16_t)d[11] << 8));
        int16_t ly = (int16_t)((uint16_t)d[12] | ((uint16_t)d[13] << 8));
        int16_t rx = (int16_t)((uint16_t)d[14] | ((uint16_t)d[15] << 8));
        int16_t ry = (int16_t)((uint16_t)d[16] | ((uint16_t)d[17] << 8));

        // Invert Y axes: use bitwise NOT (~) instead of negation to avoid
        // int16_t overflow at -32768 (-(INT16_MIN) wraps back to -32768).
        ly = (int16_t)~ly;
        ry = (int16_t)~ry;

        // RAW diagnostic: log full packet whenever buttons/triggers change
        // so we can map each button empirically.
        // Format: RAW d6=XX d7=XX bits=b15..b0 LT=XX RT=XX
        {
          static uint16_t s_lastBtns = 0xFFFF;
          static uint8_t s_lastLT = 0xFF;
          static uint8_t s_lastRT = 0xFF;
          if (xboxBtns != s_lastBtns || lt != s_lastLT || rt != s_lastRT) {
            s_lastBtns = xboxBtns;
            s_lastLT = lt;
            s_lastRT = rt;
            char bits[17];
            for (int i = 15; i >= 0; i--)
              bits[15 - i] = (xboxBtns & (1 << i)) ? '1' : '0';
            bits[16] = '\0';
            // Also dump bytes d[4..17] raw
            LOG("RAW d6=%02x d7=%02x bits=%s LT=%u RT=%u | full: %02x %02x "
                "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                d[6], d[7], bits, lt, rt, d[4], d[5], d[6], d[7], d[8], d[9],
                d[10], d[11], d[12], d[13], d[14], d[15], d[16], d[17]);
          }
        }

        // Button mapping — Xbox 360 wireless protocol (ControlStruct.h
        // big-endian, bits reversed within d[6] vs wired USB HID):
        //   d[7] = low byte:  bit0=LB, bit1=RB, bit4=A, bit5=B, bit6=X, bit7=Y
        //   d[6] = high byte (bit-reversed): bit0=RS, bit1=LS,
        //     bit2=D-Down, bit3=D-Up, bit4=Back, bit5=Start,
        //     bit6=D-Right, bit7=D-Left  (empirically verified)
        //
        // HID button order: 0=A,1=B,2=X,3=Y,4=LB,5=RB,
        //   8=Back,9=Start,10=LS,11=RS,12=D-Up,13=D-Down,14=D-Left,15=D-Right
        uint16_t hidBtns = 0;
        if (d[7] & 0x10)
          hidBtns |= (1u << 0); // A
        if (d[7] & 0x20)
          hidBtns |= (1u << 1); // B
        if (d[7] & 0x40)
          hidBtns |= (1u << 2); // X
        if (d[7] & 0x80)
          hidBtns |= (1u << 3); // Y
        if (d[7] & 0x01)
          hidBtns |= (1u << 4); // LB
        if (d[7] & 0x02)
          hidBtns |= (1u << 5); // RB
        if (d[6] & 0x10)
          hidBtns |= (1u << 8); // Back
        if (d[6] & 0x20)
          hidBtns |= (1u << 9); // Start
        // divisao de aguas
        if (d[6] & 0x08)         // direita
          hidBtns |= (1u << 14); // D-Right

        if (d[6] & 0x80) // R3
          hidBtns |= (1u << 10);

        if (d[6] & 0x01)         // cima
          hidBtns |= (1u << 11); // D-up

        if (d[6] & 0x40) // L3
          hidBtns |= (1u << 10);

        if (d[6] & 0x02)         // baixo
          hidBtns |= (1u << 12); // D-down

        if (d[6] & 0x04)         // esquerda
          hidBtns |= (1u << 13); // left

        // Build HID report
        if (s_hidBuf) {
          IOAddressSegment hrng = {};
          if (s_hidBuf->GetAddressRange(&hrng) == kIOReturnSuccess &&
              hrng.address) {
            XboxHIDReport *rpt =
                reinterpret_cast<XboxHIDReport *>(hrng.address);
            rpt->reportId = 1;
            rpt->buttons = hidBtns;
            rpt->triggerL = lt;
            rpt->triggerR = rt;
            rpt->leftX = lx;
            rpt->leftY = ly;
            rpt->rightX = rx;
            rpt->rightY = ry;

            // Dispatch to HID stack
            kern_return_t hr =
                handleReport(mach_absolute_time(), s_hidBuf,
                             sizeof(XboxHIDReport), kIOHIDReportTypeInput, 0);
            if (hr != kIOReturnSuccess) {
              LOG("handleReport FAIL 0x%x", hr);
            } else if (hidBtns || lt || rt) {
              LOG("HID OK: btns=0x%04x LT=%u RT=%u LX=%d LY=%d", hidBtns, lt,
                  rt, lx, ly);
            }
          }
        }
      }
    }
  } else if (status != kIOReturnSuccess && status != kIOReturnOverrun) {
    LOG("ReadComplete error 0x%x", status);
  }

  if (s_inPipe && s_readBuf && s_readAction) {
    kern_return_t r =
        s_inPipe->AsyncIO(s_readBuf, kReadBufSize, s_readAction, 0);
    if (r != kIOReturnSuccess)
      LOG("Re-queue failed 0x%x", r);
  }
  return;

requeue:
  if (s_inPipe && s_readBuf && s_readAction) {
    kern_return_t r =
        s_inPipe->AsyncIO(s_readBuf, kReadBufSize, s_readAction, 0);
    if (r != kIOReturnSuccess)
      LOG("Re-queue failed 0x%x", r);
  }
}

// ─────────────────────────────────────────────────────────────────────────────

void XboxReceiverDriver::WriteComplete_Impl(
    XboxReceiverDriver_WriteComplete_Args) {
  if (status == kIOReturnSuccess) {
    LOG("WriteComplete: %u bytes OK", actualByteCount);
  } else if (status != kIOReturnAborted) {
    LOG("WriteComplete error 0x%x", status);
  }
}
