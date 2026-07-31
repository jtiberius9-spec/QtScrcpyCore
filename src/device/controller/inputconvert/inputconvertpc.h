#ifndef INPUTCONVERTPC_H
#define INPUTCONVERTPC_H

#include <QByteArray>
#include <QPoint>
#include <QVector>

#include "inputconvertbase.h"

/**
 * "PC mode" -- feeds the phone a REAL HID keyboard + relative mouse over UHID,
 * instead of injecting touch events.
 *
 * Why this exists: Wine/GameNative containers expect a genuine mouse. Touch
 * injection can only ever deliver ABSOLUTE coordinates, which a PC game reads as
 * a touchscreen (you end up press-and-dragging to look around). GameNative's
 * TouchpadView.onCapturedPointer() consumes AXIS_RELATIVE_X/Y and calls
 * XServer.injectPointerMoveDelta(), so once Android sees a real HID mouse the
 * whole native mouse-look path lights up.
 *
 * Mutual exclusion with game mode is structural, not a flag: Controller swaps the
 * whole converter, so InputConvertGame's switchKey logic is not even running while
 * PC mode is active. Leaving PC mode restores the previous keymap converter.
 */
class InputConvertPC : public InputConvertBase
{
    Q_OBJECT
public:
    // UHID device ids, matching scrcpy's SC_HID_ID_KEYBOARD / SC_HID_ID_MOUSE
    enum { UHID_ID_KEYBOARD = 1, UHID_ID_MOUSE = 2 };

    explicit InputConvertPC(Controller *controller);
    virtual ~InputConvertPC();

    void mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize) override;
    void wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize) override;
    void keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize) override;

    // PC mode is not a keymap; report false so the UI does not treat it as one.
    bool isCurrentCustomKeymap() override { return false; }

private:
    void createDevices();
    void destroyDevices();
    void sendUhidInput(quint16 id, const QByteArray &report);

    void sendKeyboardReport();
    void sendMouseReport(qint8 dx, qint8 dy, qint8 wheel, qint8 hwheel);

    static quint8 hidUsageFromQtKey(int key);
    static quint8 hidModifierFromQtKey(int key);
    quint8 buttonsFromQt(Qt::MouseButtons buttons);

private:
    quint8 m_mods = 0;                 // modifier bitmask (byte 0 of the report)
    QVector<quint8> m_pressedKeys;     // HID usages currently held, max 6
    quint8 m_mouseButtons = 0;
    // no last-position / skip-flag state: the cursor is pinned at the video centre
    // every event, so each event's offset from centre IS the delta
};

#endif // INPUTCONVERTPC_H
