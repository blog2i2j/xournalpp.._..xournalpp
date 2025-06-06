//
// Created by ulrich on 06.04.19.
//

#include "InputContext.h"

#include <cstddef>  // for NULL
#include <vector>   // for vector

#include <glib-object.h>  // for g_signal_hand...

#include "control/Control.h"                            // for Control
#include "control/DeviceListHelper.h"                   // for InputDevice
#include "control/settings/Settings.h"                  // for Settings
#include "gui/XournalView.h"                            // for XournalView
#include "gui/inputdevices/GeometryToolInputHandler.h"  // for GeometryToolInputHandler
#include "gui/inputdevices/HandRecognition.h"           // for HandRecognition
#include "gui/inputdevices/KeyboardInputHandler.h"      // for KeyboardInput...
#include "gui/inputdevices/MouseInputHandler.h"         // for MouseInputHan...
#include "gui/inputdevices/StylusInputHandler.h"        // for StylusInputHa...
#include "gui/inputdevices/TouchDrawingInputHandler.h"  // for TouchDrawingI...
#include "gui/inputdevices/TouchInputHandler.h"         // for TouchInputHan...
#include "util/Assert.h"                                // for xoj_assert
#include "util/gdk4_helper.h"
#include "util/glib_casts.h"  // for wrap_for_g_callback
#include "util/gtk4_helper.h"

#include "InputEvents.h"   // for InputEvent
#include "config-debug.h"  // for DEBUG_INPUT

class ScrollHandling;
class ToolHandler;

InputContext::InputContext(XournalView* view, ScrollHandling* scrollHandling):
        view(view),
        scrollHandling(scrollHandling),
        stylusHandler(std::make_unique<StylusInputHandler>(this)),
        mouseHandler(std::make_unique<MouseInputHandler>(this)),
        touchDrawingHandler(std::make_unique<TouchDrawingInputHandler>(this)),
        keyboardHandler(std::make_unique<KeyboardInputHandler>(this)),
        touchHandler(std::make_unique<TouchInputHandler>(this)) {
    for (const InputDevice& savedDevices: this->view->getControl()->getSettings()->getKnownInputDevices()) {
        this->knownDevices.insert(savedDevices.getName());
    }
}

template <bool (KeyboardInputHandler::*handler)(KeyEvent) const>
static gboolean keyboardCallback(GtkEventControllerKey* self, guint keyval, guint, GdkModifierType state, gpointer d) {
    auto* gdkEvent = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(self));
    KeyEvent e;
    e.keyval = keyval;
    e.state = static_cast<GdkModifierType>(state & gtk_accelerator_get_default_mod_mask() &
                                           ~gdk_key_event_get_consumed_modifiers(gdkEvent));
    e.sourceEvent = gdkEvent;
    return (static_cast<KeyboardInputHandler*>(d)->*handler)(e);
}

InputContext::~InputContext() {
    // Destructor is called in xournal_widget_dispose, so it can still accept events
    g_signal_handler_disconnect(this->widget, signal_id);
}

void InputContext::connect(GtkWidget* pWidget) {
    xoj_assert(!this->widget && pWidget);
    this->widget = pWidget;
    gtk_widget_set_support_multidevice(widget, true);

#if GTK_MAJOR_VERSION == 3
    auto* keyCtrl = gtk_event_controller_key_new(widget);
#else
    auto* keyCtrl = gtk_event_controller_key_new();
    gtk_widget_add_controller(keyCtrl);
#endif

    g_signal_connect(keyCtrl, "key-pressed", G_CALLBACK(keyboardCallback<&KeyboardInputHandler::keyPressed>),
                     keyboardHandler.get());
    g_signal_connect(keyCtrl, "key-released", G_CALLBACK(keyboardCallback<&KeyboardInputHandler::keyReleased>),
                     keyboardHandler.get());

    int mask =
            // Allow scrolling
            GDK_SCROLL_MASK |

            // Touch / Pen / Mouse
            GDK_TOUCH_MASK | GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
            GDK_SMOOTH_SCROLL_MASK | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK | GDK_PROXIMITY_IN_MASK |
            GDK_PROXIMITY_OUT_MASK;

    gtk_widget_add_events(pWidget, mask);


    signal_id = g_signal_connect(pWidget, "event", xoj::util::wrap_for_g_callback_v<eventCallback>, this);
}

auto InputContext::eventCallback(GtkWidget* widget, GdkEvent* event, InputContext* self) -> bool {
    return self->handle(event);
}

auto InputContext::handle(GdkEvent* sourceEvent) -> bool {
    printDebug(sourceEvent);

    GdkDevice* sourceDevice = gdk_event_get_source_device(sourceEvent);
    if (sourceDevice == NULL) {
        return false;
    }

    InputEvent event = InputEvents::translateEvent(sourceEvent, this->getSettings());

    // Add the device to the list of known devices if it is currently unknown
    GdkInputSource inputSource = gdk_device_get_source(sourceDevice);
    if (inputSource != GDK_SOURCE_KEYBOARD && gdk_device_get_device_type(sourceDevice) != GDK_DEVICE_TYPE_MASTER &&
        this->knownDevices.find(std::string(event.deviceName)) == this->knownDevices.end()) {

        this->knownDevices.insert(std::string(event.deviceName));
        this->getSettings()->transactionStart();
        auto deviceClassOption =
                this->getSettings()->getDeviceClassForDevice(std::string(event.deviceName), inputSource);
        this->getSettings()->setDeviceClassForDevice(sourceDevice, deviceClassOption);
        this->getSettings()->transactionEnd();
    }

    // We do not handle scroll events manually but let GTK do it for us
    if (event.type == SCROLL_EVENT) {
        // Hand over to standard GTK Scroll / Zoom handling
        return false;
    }

    // Deactivate touchscreen when a pen event occurs
    this->getView()->getHandRecognition()->event(event.deviceClass);

    // separate events to appropriate handlers
    // handle geometry tool
    if (geometryToolInputHandler && geometryToolInputHandler->handle(event)) {
        return true;
    }

    // handle tablet stylus
    if (event.deviceClass == INPUT_DEVICE_PEN || event.deviceClass == INPUT_DEVICE_ERASER) {
        return this->stylusHandler->handle(event);
    }

    // handle mouse devices
    if (event.deviceClass == INPUT_DEVICE_MOUSE || event.deviceClass == INPUT_DEVICE_MOUSE_KEYBOARD_COMBO) {
        return this->mouseHandler->handle(event);
    }

    // handle touchscreens
    if (event.deviceClass == INPUT_DEVICE_TOUCHSCREEN) {
        bool touchDrawingEnabled = this->getSettings()->getTouchDrawingEnabled();

        // trigger touch drawing depending on the setting
        if (touchDrawingEnabled) {
            return this->touchDrawingHandler->handle(event) || this->touchHandler->handle(event);
        }

        return this->touchHandler->handle(event);
    }

    if (event.deviceClass == INPUT_DEVICE_IGNORE) {
        return true;
    }

#ifdef DEBUG_INPUT
    if (event.deviceClass != INPUT_DEVICE_KEYBOARD) {  // Keyboard event are handled via the GtkEventControllerKey
        g_message("We received an event we do not have a handler for");
    }
#endif
    return false;
}

auto InputContext::getXournal() -> GtkXournal* { return GTK_XOURNAL(widget); }

auto InputContext::getView() -> XournalView* { return view; }

auto InputContext::getSettings() -> Settings* { return view->getControl()->getSettings(); }

auto InputContext::getToolHandler() -> ToolHandler* { return view->getControl()->getToolHandler(); }

auto InputContext::getScrollHandling() -> ScrollHandling* { return this->scrollHandling; }

void InputContext::setGeometryToolInputHandler(std::unique_ptr<GeometryToolInputHandler> handler) {
    this->geometryToolInputHandler = std::move(handler);
}

auto InputContext::getGeometryToolInputHandler() const -> GeometryToolInputHandler* {
    return geometryToolInputHandler.get();
}

void InputContext::resetGeometryToolInputHandler() { this->geometryToolInputHandler.reset(); }

/**
 * Focus the widget
 */
void InputContext::focusWidget() {
    if (!gtk_widget_has_focus(widget)) {
        gtk_widget_grab_focus(widget);
    }
}

void InputContext::blockDevice(InputContext::DeviceType deviceType) {
    if (geometryToolInputHandler) {
        geometryToolInputHandler->blockDevice(deviceType);
    }
    switch (deviceType) {
        case MOUSE:
            this->mouseHandler->block(true);
            break;
        case STYLUS:
            this->stylusHandler->block(true);
            break;
        case TOUCHSCREEN:
            this->touchDrawingHandler->block(true);
            this->touchHandler->block(true);
            break;
    }
}

void InputContext::unblockDevice(InputContext::DeviceType deviceType) {
    if (geometryToolInputHandler) {
        geometryToolInputHandler->unblockDevice(deviceType);
    }
    switch (deviceType) {
        case MOUSE:
            this->mouseHandler->block(false);
            break;
        case STYLUS:
            this->stylusHandler->block(false);
            break;
        case TOUCHSCREEN:
            this->touchDrawingHandler->block(false);
            this->touchHandler->block(false);
            break;
    }
}

auto InputContext::isBlocked(InputContext::DeviceType deviceType) -> bool {
    switch (deviceType) {
        case MOUSE:
            return this->mouseHandler->isBlocked();
        case STYLUS:
            return this->stylusHandler->isBlocked();
        case TOUCHSCREEN:
            return this->touchDrawingHandler->isBlocked();
    }
    return false;
}

void InputContext::printDebug(GdkEvent* event) {
#ifdef DEBUG_INPUT_GDK_PRINT_EVENTS
    gdk_set_show_events(true);
#else
#ifdef DEBUG_INPUT
    std::string message = "Event\n";
    std::string gdkEventTypes[] = {"GDK_NOTHING",
                                   "GDK_DELETE",
                                   "GDK_DESTROY",
                                   "GDK_EXPOSE",
                                   "GDK_MOTION_NOTIFY",
                                   "GDK_BUTTON_PRESS",
                                   "GDK_DOUBLE_BUTTON_PRESS",
                                   "GDK_TRIPLE_BUTTON_PRESS",
                                   "GDK_BUTTON_RELEASE",
                                   "GDK_KEY_PRESS",
                                   "GDK_KEY_RELEASE",
                                   "GDK_ENTER_NOTIFY",
                                   "GDK_LEAVE_NOTIFY",
                                   "GDK_FOCUS_CHANGE",
                                   "GDK_CONFIGURE",
                                   "GDK_MAP",
                                   "GDK_UNMAP",
                                   "GDK_PROPERTY_NOTIFY",
                                   "GDK_SELECTION_CLEAR",
                                   "GDK_SELECTION_REQUEST",
                                   "GDK_SELECTION_NOTIFY",
                                   "GDK_PROXIMITY_IN",
                                   "GDK_PROXIMITY_OUT",
                                   "GDK_DRAG_ENTER",
                                   "GDK_DRAG_LEAVE",
                                   "GDK_DRAG_MOTION",
                                   "GDK_DRAG_STATUS",
                                   "GDK_DROP_START",
                                   "GDK_DROP_FINISHED",
                                   "GDK_CLIENT_EVENT",
                                   "GDK_VISIBILITY_NOTIFY",
                                   "",
                                   "GDK_SCROLL",
                                   "GDK_WINDOW_STATE",
                                   "GDK_SETTING",
                                   "GDK_OWNER_CHANGE",
                                   "GDK_GRAB_BROKEN",
                                   "GDK_DAMAGE",
                                   "GDK_TOUCH_BEGIN",
                                   "GDK_TOUCH_UPDATE",
                                   "GDK_TOUCH_END",
                                   "GDK_TOUCH_CANCEL",
                                   "GDK_TOUCHPAD_SWIPE",
                                   "GDK_TOUCHPAD_PINCH",
                                   "GDK_PAD_BUTTON_PRESS",
                                   "GDK_PAD_BUTTON_RELEASE",
                                   "GDK_PAD_RING",
                                   "GDK_PAD_STRIP",
                                   "GDK_PAD_GROUP_MODE",
                                   "GDK_EVENT_LAST"};
    message += "Event type:\t" + gdkEventTypes[gdk_event_get_event_type(event) + 1] + "\n";

    std::string gdkInputSources[] = {"GDK_SOURCE_MOUSE",    "GDK_SOURCE_PEN",        "GDK_SOURCE_ERASER",
                                     "GDK_SOURCE_CURSOR",   "GDK_SOURCE_KEYBOARD",   "GDK_SOURCE_TOUCHSCREEN",
                                     "GDK_SOURCE_TOUCHPAD", "GDK_SOURCE_TRACKPOINT", "GDK_SOURCE_TABLET_PAD"};
    GdkDevice* device = gdk_event_get_source_device(event);
    message += "Source device:\t" + gdkInputSources[gdk_device_get_source(device)] + "\n";
    std::string gdkInputClasses[] = {"INPUT_DEVICE_MOUSE",    "INPUT_DEVICE_PEN",
                                     "INPUT_DEVICE_ERASER",   "INPUT_DEVICE_TOUCHSCREEN",
                                     "INPUT_DEVICE_KEYBOARD", "INPUT_DEVICE_MOUSE_KEYBOARD_COMBO",
                                     "INPUT_DEVICE_IGNORE"};
    InputDeviceClass deviceClass = InputEvents::translateDeviceType(device, this->getSettings());
    message += "Device Class:\t" + gdkInputClasses[deviceClass] + "\n";

    if (gdk_event_get_event_type(event) == GDK_BUTTON_PRESS ||
        gdk_event_get_event_type(event) == GDK_DOUBLE_BUTTON_PRESS ||
        gdk_event_get_event_type(event) == GDK_TRIPLE_BUTTON_PRESS ||
        gdk_event_get_event_type(event) == GDK_BUTTON_RELEASE) {
        guint button;
        if (gdk_event_get_button(event, &button)) {
            message += "Button:\t" + std::to_string(button) + "\n";
        }
    }

#ifndef DEBUG_INPUT_PRINT_ALL_MOTION_EVENTS
    static bool motionEventBlock = false;
    if (gdk_event_get_event_type(event) == GDK_MOTION_NOTIFY) {
        if (!motionEventBlock) {
            motionEventBlock = true;
            g_message("%s", message.c_str());
        }
    } else {
        motionEventBlock = false;
        g_message("%s", message.c_str());
    }
#else
    g_message("%s", message.c_str());
#endif  // DEBUG_INPUT_PRINT_ALL_MOTION_EVENTS
#endif  // DEBUG_INPUT
#endif  // DEBUG_INPUT_PRINT_EVENTS
}
