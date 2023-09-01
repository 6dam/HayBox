#include "comms/B0XXInputViewer.hpp"
#include "comms/DInputBackend.hpp"
#include "comms/GamecubeBackend.hpp"
#include "comms/N64Backend.hpp"
#include "comms/NintendoSwitchBackend.hpp"
#include "comms/XInputBackend.hpp"
#include "config/mode_selection.hpp"
#include "core/CommunicationBackend.hpp"
#include "core/InputMode.hpp"
#include "core/KeyboardMode.hpp"
#include "core/pinout.hpp"
#include "core/socd.hpp"
#include "core/state.hpp"
#include "input/GpioButtonInput.hpp"
#include "input/NunchukInput.hpp"
#include "joybus_utils.hpp"
#include "modes/Melee20Button.hpp"
#include "stdlib.hpp"

#include <pico/bootrom.h>

//OLED stuff
#include <lib/OneBitDisplay/OneBitDisplay.h>
#include <string>
#include <cstring>
std::string dispCommBackend = "BACKEND";
std::string dispMode = "           MODE";
std::string statusBar;

CommunicationBackend **backends = nullptr;
size_t backend_count;
KeyboardMode *current_kb_mode = nullptr;

GpioButtonMapping button_mappings[] = {
    {&InputState::l,            5 },
    { &InputState::left,        4 },
    { &InputState::down,        3 },
    { &InputState::right,       2 },

    { &InputState::mod_x,       6 },
    { &InputState::mod_y,       7 },

    { &InputState::select,      10},
    { &InputState::start,       0 },
    { &InputState::home,        11},

    { &InputState::c_left,      13},
    { &InputState::c_up,        12},
    { &InputState::c_down,      15},
    { &InputState::a,           14},
    { &InputState::c_right,     16},

    { &InputState::b,           26},
    { &InputState::x,           21},
    { &InputState::z,           19},
    { &InputState::up,          17},

    { &InputState::r,           27},
    { &InputState::y,           22},
    { &InputState::lightshield, 20},
    { &InputState::midshield,   18},
};
size_t button_count = sizeof(button_mappings) / sizeof(GpioButtonMapping);

const Pinout pinout = {
    .joybus_data = 28,
    .mux = -1,
    .nunchuk_detect = -1,
    .nunchuk_sda = -1,
    .nunchuk_scl = -1,
};

void setup() {
    // Create GPIO input source and use it to read button states for checking button holds.
    GpioButtonInput *gpio_input = new GpioButtonInput(button_mappings, button_count);

    InputState button_holds;
    gpio_input->UpdateInputs(button_holds);

    // Bootsel button hold as early as possible for safety.
    if (button_holds.start) {
        reset_usb_boot(0, 0);
    }

    // Turn on LED to indicate firmware booted.
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    // Create array of input sources to be used.
    static InputSource *input_sources[] = { gpio_input };
    size_t input_source_count = sizeof(input_sources) / sizeof(InputSource *);

    ConnectedConsole console = detect_console(pinout.joybus_data);

    /* Select communication backend. */
    CommunicationBackend *primary_backend;
    if (console == ConnectedConsole::NONE) {
        if (button_holds.x) {
            // If no console detected and X is held on plugin then use Switch USB backend.
            NintendoSwitchBackend::RegisterDescriptor();
            backend_count = 1;
            primary_backend = new NintendoSwitchBackend(input_sources, input_source_count);
            backends = new CommunicationBackend *[backend_count] { primary_backend };
            //dispCommBackend = "SWITCH"; //Display isn't working properly when holding X on plugin.

            // Default to Ultimate mode on Switch.
            primary_backend->SetGameMode(new Ultimate(socd::SOCD_2IP));
            return;
        } else if (button_holds.z) {
            // If no console detected and Z is held on plugin then use DInput backend.
            TUGamepad::registerDescriptor();
            TUKeyboard::registerDescriptor();
            backend_count = 2;
            primary_backend = new DInputBackend(input_sources, input_source_count);
            backends = new CommunicationBackend *[backend_count] {
                primary_backend, new B0XXInputViewer(input_sources, input_source_count)
            };
            dispCommBackend = "DINPUT";

        } else {
            // Default to XInput mode if no console detected and no other mode forced.
            backend_count = 2;
            primary_backend = new XInputBackend(input_sources, input_source_count);
            backends = new CommunicationBackend *[backend_count] {
                primary_backend, new B0XXInputViewer(input_sources, input_source_count)
            };
            dispCommBackend = "XINPUT";
        }
    } else {
        if (console == ConnectedConsole::GAMECUBE) {
            primary_backend =
                new GamecubeBackend(input_sources, input_source_count, pinout.joybus_data);
            dispCommBackend = "GCN"; // Not yet tested.
        } else if (console == ConnectedConsole::N64) {
            primary_backend = new N64Backend(input_sources, input_source_count, pinout.joybus_data);
            dispCommBackend = "N64"; // Not yet tested.
        }

        // If console then only using 1 backend (no input viewer).
        backend_count = 1;
        backends = new CommunicationBackend *[backend_count] { primary_backend };
    }

    // Default to Melee mode.
    primary_backend->SetGameMode(
        new Melee20Button(socd::SOCD_2IP_NO_REAC, { .crouch_walk_os = false })
    );
    dispMode = "          MELEE";
}

void loop() {
    select_mode(backends[0]);

    for (size_t i = 0; i < backend_count; i++) {
        backends[i]->SendReport();
    }

    if (current_kb_mode != nullptr) {
        current_kb_mode->SendReport(backends[0]->GetInputs());
    }
}

/* Nunchuk code runs on the second core */
NunchukInput *nunchuk = nullptr;

// OLED display Setup

#ifndef DISPLAY_I2C_ADDR
#define DISPLAY_I2C_ADDR -1 
#endif

#ifndef I2C_SDA_PIN
#define I2C_SDA_PIN 10 //Set depending on what you hook up your display to.
#endif

#ifndef I2C_SCL_PIN
#define I2C_SCL_PIN 11 //Set depending on what you hook up your display to.
#endif

#ifndef I2C_BLOCK
#define I2C_BLOCK i2c1 //Set depending on which pins selected.
#endif

//SDA Pin,SCL Pin,I2C Block
//0,1,i2c0
//2,3,i2c1
//4,5,i2c0
//6,7,i2c1
//8,9,i2c0
//10,11,i2c1
//12,13,i2c0
//14,15,i2c1
//16,17,i2c0
//18,19,i2c1
//20,21,i2c0
//26,27,i2c1

#ifndef I2C_SPEED
#define I2C_SPEED 800000 //Common values are 100000 for standard, 400000 for fast and 800000 ludicrous speed.
#endif

#ifndef DISPLAY_SIZE
#define DISPLAY_SIZE OLED_128x64 //Can be changed to match different displays.
#endif

#ifndef DISPLAY_FLIP
#define DISPLAY_FLIP 0 
#endif

#ifndef DISPLAY_INVERT
#define DISPLAY_INVERT 0
#endif

#ifndef DISPLAY_USEWIRE
#define DISPLAY_USEWIRE 1
#endif

OBDISP obd;
uint8_t ucBackBuffer[1024];


void setup1() {
    while (backends == nullptr) {
        tight_loop_contents();
    }

    // Create Nunchuk input source.
    nunchuk = new NunchukInput(Wire, pinout.nunchuk_detect, pinout.nunchuk_sda, pinout.nunchuk_scl);

        //Initialize OLED.
    	obdI2CInit(&obd,
	    DISPLAY_SIZE,
		DISPLAY_I2C_ADDR,
		DISPLAY_FLIP,
		DISPLAY_INVERT,
		DISPLAY_USEWIRE,
		I2C_SDA_PIN,
		I2C_SCL_PIN,
		I2C_BLOCK,
        -1,
		I2C_SPEED);

        
	    obdSetBackBuffer(&obd, ucBackBuffer);
        //Clear screen and render.
        obdFill(&obd, 0, 1);
}

int counter = 0;

void loop1() {
    if (backends != nullptr) {
        nunchuk->UpdateInputs(backends[0]->GetInputs());
        busy_wait_us(50);
    }

    //Clear screen but don't send to render yet.
    obdFill(&obd, 0, 0);

    //Set mode string based on input combination. Couldn't figure out how to read the current mode, that'd be better.
    if (backends[0]->GetInputs().mod_x == true && backends[0]->GetInputs().mod_y == false && backends[0]->GetInputs().start == true) {
        if (backends[0]->GetInputs().l) {
            dispMode = "          MELEE";
        } else if (backends[0]->GetInputs().left) {
            dispMode = "             PM";
        } else if (backends[0]->GetInputs().down) {
            dispMode = "            ULT";
        } else if (backends[0]->GetInputs().right) {
            dispMode = "            FGC";
        } else if (backends[0]->GetInputs().b) {
            dispMode = "            RoA";
        }
    }
    
    // Combine communications backed and mode into one status bar.
    statusBar = dispCommBackend + dispMode;

    // Write statusbar to OLED starting in the top left. For some reason disappears after 4414 loops? No idea why. Works fine until that point.
    char * char_statusBar = new char[statusBar.length() + 1];
    strcpy(char_statusBar, statusBar.c_str()); //convert string to char
    obdWriteString(&obd, 0, 0, 0, char_statusBar, FONT_6x8, 0, 0);
    
    // Counter displayed in bottom left for troubleshooting text timeout.
    counter += 1;
    std::string strCounter = std::to_string(counter);
    char * char_strCounter = new char[strCounter.length() + 1];
    strcpy(char_strCounter, strCounter.c_str());
    obdWriteString(&obd, 0, 0, 7, char_strCounter, FONT_6x8, 0, 0);

    // Draw buttons.
    obdPreciseEllipse(&obd, 6,  29, 4, 4, 1, backends[0]->GetInputs().l);
    obdPreciseEllipse(&obd, 15, 23, 4, 4, 1, backends[0]->GetInputs().left);
    obdPreciseEllipse(&obd, 25, 22, 4, 4, 1, backends[0]->GetInputs().down);
    obdPreciseEllipse(&obd, 35, 27, 4, 4, 1, backends[0]->GetInputs().right);
    obdPreciseEllipse(&obd, 38, 51, 4, 4, 1, backends[0]->GetInputs().mod_x);
    obdPreciseEllipse(&obd, 46, 57, 4, 4, 1, backends[0]->GetInputs().mod_y);
    obdPreciseEllipse(&obd, 64, 27, 4, 4, 1, backends[0]->GetInputs().start);
    obdPreciseEllipse(&obd, 82,  46, 4, 4, 1, backends[0]->GetInputs().c_left);
    obdPreciseEllipse(&obd, 82,  57, 4, 4, 1, backends[0]->GetInputs().c_down);
    obdPreciseEllipse(&obd, 90,  40, 4, 4, 1, backends[0]->GetInputs().c_up);
    obdPreciseEllipse(&obd, 90,  52, 4, 4, 1, backends[0]->GetInputs().a);
    obdPreciseEllipse(&obd, 99,  46, 4, 4, 1, backends[0]->GetInputs().c_right);
    obdPreciseEllipse(&obd, 93, 17, 4, 4, 1, backends[0]->GetInputs().r);
    obdPreciseEllipse(&obd, 93,  27, 4, 4, 1, backends[0]->GetInputs().b);
    obdPreciseEllipse(&obd, 103, 13, 4, 4, 1, backends[0]->GetInputs().y);
    obdPreciseEllipse(&obd, 102, 23, 4, 4, 1, backends[0]->GetInputs().x);
    obdPreciseEllipse(&obd, 113, 14, 4, 4, 1, backends[0]->GetInputs().lightshield);
    obdPreciseEllipse(&obd, 112, 24, 4, 4, 1, backends[0]->GetInputs().z);
    obdPreciseEllipse(&obd, 122, 19, 4, 4, 1, backends[0]->GetInputs().midshield);
    obdPreciseEllipse(&obd, 122, 29, 4, 4, 1, backends[0]->GetInputs().up);

    obdDumpBuffer(&obd,NULL); //Sends buffered content to display.
}