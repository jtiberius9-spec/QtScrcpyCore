#include <QCursor>
#include <QDebug>
#include <QtMath>

#include "controller.h"
#include "inputconvertpc.h"

// HID report descriptors lifted verbatim from scrcpy v3.3.3
// (app/src/hid/hid_keyboard.c and hid_mouse.c) so the bundled 3.3.3 server and
// the Android HID stack see exactly the devices they already know how to handle.

// Boot-protocol keyboard: 8-byte reports -> [mods][reserved][6 key usages]
static const quint8 HID_KEYBOARD_REPORT_DESC[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x06,       // Usage (Keyboard)
    0xA1, 0x01,       // Collection (Application)
    0x05, 0x07,       //   Usage Page (Key Codes)
    0x19, 0xE0,       //   Usage Minimum (224)
    0x29, 0xE7,       //   Usage Maximum (231)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x01,       //   Logical Maximum (1)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x08,       //   Report Count (8)
    0x81, 0x02,       //   Input (Data,Var,Abs): modifier byte
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x01,       //   Input (Const): reserved byte
    0x05, 0x08,       //   Usage Page (LEDs)
    0x19, 0x01,       //   Usage Minimum (1)
    0x29, 0x05,       //   Usage Maximum (5)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x05,       //   Report Count (5)
    0x91, 0x02,       //   Output (Data,Var,Abs): LED report
    0x75, 0x03,       //   Report Size (3)
    0x95, 0x01,       //   Report Count (1)
    0x91, 0x01,       //   Output (Const): LED padding
    0x05, 0x07,       //   Usage Page (Key Codes)
    0x19, 0x00,       //   Usage Minimum (0)
    0x29, 0x65,       //   Usage Maximum (101)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x65,       //   Logical Maximum (101)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x06,       //   Report Count (6)
    0x81, 0x00,       //   Input (Data,Array): 6 keys
    0xC0              // End Collection
};

// Relative mouse: 5-byte reports -> [buttons][dx][dy][wheel][hwheel]
static const quint8 HID_MOUSE_REPORT_DESC[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x02,       // Usage (Mouse)
    0xA1, 0x01,       // Collection (Application)
    0x09, 0x01,       //   Usage (Pointer)
    0xA1, 0x00,       //   Collection (Physical)
    0x05, 0x09,       //     Usage Page (Buttons)
    0x19, 0x01,       //     Usage Minimum (1)
    0x29, 0x05,       //     Usage Maximum (5)
    0x15, 0x00,       //     Logical Minimum (0)
    0x25, 0x01,       //     Logical Maximum (1)
    0x95, 0x05,       //     Report Count (5)
    0x75, 0x01,       //     Report Size (1)
    0x81, 0x02,       //     Input (Data,Var,Abs): 5 button bits
    0x95, 0x01,       //     Report Count (1)
    0x75, 0x03,       //     Report Size (3)
    0x81, 0x01,       //     Input (Const): 3 bits padding
    0x05, 0x01,       //     Usage Page (Generic Desktop)
    0x09, 0x30,       //     Usage (X)
    0x09, 0x31,       //     Usage (Y)
    0x09, 0x38,       //     Usage (Wheel)
    0x15, 0x81,       //     Logical Minimum (-127)
    0x25, 0x7F,       //     Logical Maximum (127)
    0x75, 0x08,       //     Report Size (8)
    0x95, 0x03,       //     Report Count (3)
    0x81, 0x06,       //     Input (Data,Var,Rel): X, Y, Wheel
    0x05, 0x0C,       //     Usage Page (Consumer)
    0x0A, 0x38, 0x02, //     Usage (AC Pan)
    0x15, 0x81,       //     Logical Minimum (-127)
    0x25, 0x7F,       //     Logical Maximum (127)
    0x75, 0x08,       //     Report Size (8)
    0x95, 0x01,       //     Report Count (1)
    0x81, 0x06,       //     Input (Data,Var,Rel): AC Pan
    0xC0,             //   End Collection
    0xC0              // End Collection
};

#define HID_MOD_LEFT_CONTROL (1 << 0)
#define HID_MOD_LEFT_SHIFT   (1 << 1)
#define HID_MOD_LEFT_ALT     (1 << 2)
#define HID_MOD_LEFT_GUI     (1 << 3)

#define HID_KEYBOARD_MAX_KEYS 6

InputConvertPC::InputConvertPC(Controller *controller) : InputConvertBase(controller)
{
    createDevices();
    // NOTE: do NOT emit grabCursor/recoilHint from here -- Controller connects our
    // signals only AFTER construction, so anything emitted in the ctor is dropped.
    // VideoForm's F7 handler owns the cursor grab and the on-screen notice instead.
}

InputConvertPC::~InputConvertPC()
{
    destroyDevices();
}

void InputConvertPC::createDevices()
{
    // BOTH devices are deliberately named "scrcpy", for two measured reasons:
    //
    // 1. Android merges input devices that share a descriptor (same bus/vendor/
    //    product/name) into ONE InputDevice with Sources = KEYBOARD | MOUSE. That
    //    is exactly what upstream scrcpy produces, and it is the configuration
    //    proven to work on this phone. Giving them distinct names created two
    //    devices and Android registered only the keyboard -- the mouse never
    //    appeared in `dumpsys input` at all.
    // 2. GameNative special-cases the name: XServerScreen.kt does
    //    `if (device?.name == "scrcpy") usingScreenMirror = true`, which suppresses
    //    its on-screen touch controls. Any other name leaves the gamepad overlay
    //    drawn and fighting for input.
    static const QString kUhidName = QStringLiteral("scrcpy");

    ControlMsg *kb = new ControlMsg(ControlMsg::CMT_UHID_CREATE);
    kb->setUhidCreateData(
        UHID_ID_KEYBOARD, 0, 0, kUhidName,
        QByteArray(reinterpret_cast<const char *>(HID_KEYBOARD_REPORT_DESC), sizeof(HID_KEYBOARD_REPORT_DESC)));
    sendControlMsg(kb);

    ControlMsg *ms = new ControlMsg(ControlMsg::CMT_UHID_CREATE);
    ms->setUhidCreateData(
        UHID_ID_MOUSE, 0, 0, kUhidName,
        QByteArray(reinterpret_cast<const char *>(HID_MOUSE_REPORT_DESC), sizeof(HID_MOUSE_REPORT_DESC)));
    sendControlMsg(ms);
}

void InputConvertPC::destroyDevices()
{
    // release every held key first, or the device is left with keys stuck down
    m_mods = 0;
    m_pressedKeys.clear();
    sendKeyboardReport();
    m_mouseButtons = 0;
    sendMouseReport(0, 0, 0, 0);

    for (quint16 id : { (quint16)UHID_ID_KEYBOARD, (quint16)UHID_ID_MOUSE }) {
        ControlMsg *msg = new ControlMsg(ControlMsg::CMT_UHID_DESTROY);
        msg->setUhidDestroyData(id);
        sendControlMsg(msg);
    }
}

void InputConvertPC::sendUhidInput(quint16 id, const QByteArray &report)
{
    ControlMsg *msg = new ControlMsg(ControlMsg::CMT_UHID_INPUT);
    msg->setUhidInputData(id, report);
    sendControlMsg(msg);
}

// (report layout verified 2026-08-01: [mods][reserved][6 keys], boot-keyboard standard)
void InputConvertPC::sendKeyboardReport()
{
    QByteArray report(2 + HID_KEYBOARD_MAX_KEYS, '\0');
    report[0] = static_cast<char>(m_mods);
    report[1] = '\0'; // reserved
    for (int i = 0; i < m_pressedKeys.size() && i < HID_KEYBOARD_MAX_KEYS; ++i) {
        report[2 + i] = static_cast<char>(m_pressedKeys.at(i));
    }
    sendUhidInput(UHID_ID_KEYBOARD, report);
}

void InputConvertPC::sendMouseReport(qint8 dx, qint8 dy, qint8 wheel, qint8 hwheel)
{
    QByteArray report(5, '\0');
    report[0] = static_cast<char>(m_mouseButtons);
    report[1] = static_cast<char>(dx);
    report[2] = static_cast<char>(dy);
    report[3] = static_cast<char>(wheel);
    report[4] = static_cast<char>(hwheel);
    sendUhidInput(UHID_ID_MOUSE, report);
}

quint8 InputConvertPC::hidModifierFromQtKey(int key)
{
    // ponytail: Qt::Key_Control/Shift/Alt/Meta do not distinguish left from right,
    // so everything maps to the LEFT modifier. Games do not care; if a right-hand
    // modifier ever needs its own usage, read nativeVirtualKey() on Windows.
    switch (key) {
    case Qt::Key_Control: return HID_MOD_LEFT_CONTROL;
    case Qt::Key_Shift:   return HID_MOD_LEFT_SHIFT;
    case Qt::Key_Alt:     return HID_MOD_LEFT_ALT;
    case Qt::Key_Meta:    return HID_MOD_LEFT_GUI;
    default:              return 0;
    }
}

quint8 InputConvertPC::hidUsageFromQtKey(int key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        return static_cast<quint8>(0x04 + (key - Qt::Key_A));
    }
    if (key >= Qt::Key_1 && key <= Qt::Key_9) {
        return static_cast<quint8>(0x1E + (key - Qt::Key_1));
    }
    if (key == Qt::Key_0) {
        return 0x27;
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12) {
        return static_cast<quint8>(0x3A + (key - Qt::Key_F1));
    }
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:       return 0x28;
    case Qt::Key_Escape:      return 0x29;
    case Qt::Key_Backspace:   return 0x2A;
    case Qt::Key_Tab:         return 0x2B;
    case Qt::Key_Space:       return 0x2C;
    case Qt::Key_Minus:       return 0x2D;
    case Qt::Key_Equal:       return 0x2E;
    case Qt::Key_BracketLeft: return 0x2F;
    case Qt::Key_BracketRight:return 0x30;
    case Qt::Key_Backslash:   return 0x31;
    case Qt::Key_Semicolon:   return 0x33;
    case Qt::Key_Apostrophe:  return 0x34;
    case Qt::Key_QuoteLeft:   return 0x35;
    case Qt::Key_Comma:       return 0x36;
    case Qt::Key_Period:      return 0x37;
    case Qt::Key_Slash:       return 0x38;
    case Qt::Key_CapsLock:    return 0x39;
    case Qt::Key_Print:       return 0x46;
    case Qt::Key_ScrollLock:  return 0x47;
    case Qt::Key_Pause:       return 0x48;
    case Qt::Key_Insert:      return 0x49;
    case Qt::Key_Home:        return 0x4A;
    case Qt::Key_PageUp:      return 0x4B;
    case Qt::Key_Delete:      return 0x4C;
    case Qt::Key_End:         return 0x4D;
    case Qt::Key_PageDown:    return 0x4E;
    case Qt::Key_Right:       return 0x4F;
    case Qt::Key_Left:        return 0x50;
    case Qt::Key_Down:        return 0x51;
    case Qt::Key_Up:          return 0x52;
    case Qt::Key_NumLock:     return 0x53;
    default:                  return 0;
    }
}

void InputConvertPC::keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize)
{
    Q_UNUSED(frameSize)
    Q_UNUSED(showSize)

    if (from->isAutoRepeat()) {
        return; // HID holds the key down on its own; repeats would spam reports
    }

    const bool down = (from->type() == QEvent::KeyPress);

    quint8 mod = hidModifierFromQtKey(from->key());
    if (mod) {
        if (down) {
            m_mods |= mod;
        } else {
            m_mods &= ~mod;
        }
        sendKeyboardReport();
        return;
    }

    quint8 usage = hidUsageFromQtKey(from->key());
    if (!usage) {
        return;
    }

    if (down) {
        if (!m_pressedKeys.contains(usage)) {
            if (m_pressedKeys.size() >= HID_KEYBOARD_MAX_KEYS) {
                m_pressedKeys.removeFirst(); // 6-key rollover, drop the oldest
            }
            m_pressedKeys.append(usage);
        }
    } else {
        m_pressedKeys.removeAll(usage);
    }
    sendKeyboardReport();
}

quint8 InputConvertPC::buttonsFromQt(Qt::MouseButtons buttons)
{
    quint8 c = 0;
    if (buttons & Qt::LeftButton)   c |= 1 << 0;
    if (buttons & Qt::RightButton)  c |= 1 << 1;
    if (buttons & Qt::MiddleButton) c |= 1 << 2;
    if (buttons & Qt::XButton1)     c |= 1 << 3;
    if (buttons & Qt::XButton2)     c |= 1 << 4;
    return c;
}

void InputConvertPC::mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize)
{
    Q_UNUSED(frameSize)

    m_mouseButtons = buttonsFromQt(from->buttons());

    if (from->type() != QEvent::MouseMove) {
        sendMouseReport(0, 0, 0, 0); // press / release: buttons only
        return;
    }

    // TRUE relative mouse, the way SDL's relative mode behaves: the cursor is
    // PINNED at the centre of the video every single event, so an event's offset
    // from the centre *is* the delta and the pointer never travels.
    //
    // Do NOT go back to differencing successive positions and re-anchoring only
    // near the edges: the cursor then drifts across the window and each re-anchor
    // is a visible SNAP BACK mid-turn. That is the exact flick that took days to
    // kill in the Python Wraith's aim code -- same mistake, same symptom.
    //
    // A bonus of measuring from the centre: our own warp lands exactly on the
    // centre, so it yields a zero delta and is ignored for free. No skip flag.
    const QPoint centre(showSize.width() / 2, showSize.height() / 2);
    const QPoint delta = from->pos() - centre;

    if (!delta.isNull()) {
        // One HID report carries at most +/-127 per axis, so a fast flick has to
        // be split across several reports or the turn would be clamped and feel
        // sluggish exactly when precision matters most.
        int rx = delta.x();
        int ry = delta.y();
        while (rx != 0 || ry != 0) {
            int sx = qBound(-127, rx, 127);
            int sy = qBound(-127, ry, 127);
            sendMouseReport(static_cast<qint8>(sx), static_cast<qint8>(sy), 0, 0);
            rx -= sx;
            ry -= sy;
        }

        // Put the cursor straight back on the centre so the next event measures a
        // fresh delta and we can never reach the clip edge.
        const QPoint origin = from->globalPos() - from->pos(); // widget origin, global
        QCursor::setPos(origin + centre);
    }
}

void InputConvertPC::wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize)
{
    Q_UNUSED(frameSize)
    Q_UNUSED(showSize)

    // HID wheel is integral clicks; Qt reports eighths of a degree (120 per notch)
    const int v = from->angleDelta().y() / 120;
    const int h = from->angleDelta().x() / 120;
    if (v == 0 && h == 0) {
        return;
    }
    sendMouseReport(0, 0, static_cast<qint8>(qBound(-127, v, 127)), static_cast<qint8>(qBound(-127, h, 127)));
}
