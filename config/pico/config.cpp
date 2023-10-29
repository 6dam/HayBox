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
std::string dispMode = "MODE";
std::string leftLayout;
std::string centerLayout;
std::string rightLayout;

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
            dispCommBackend = "SWITCH"; //Display isn't refreshing properly when holding X on plugin and connecting to PC.

            // Default to Ultimate mode on Switch.
            primary_backend->SetGameMode(new Ultimate(socd::SOCD_2IP));
            dispMode = "ULT";
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
            dispCommBackend = "GCN";
        } else if (console == ConnectedConsole::N64) {
            primary_backend = new N64Backend(input_sources, input_source_count, pinout.joybus_data);
            dispCommBackend = "N64";
        }

        // If console then only using 1 backend (no input viewer).
        backend_count = 1;
        backends = new CommunicationBackend *[backend_count] { primary_backend };
    }

    // Default to Melee mode.
    primary_backend->SetGameMode(
        new Melee20Button(socd::SOCD_2IP_NO_REAC, { .crouch_walk_os = false })
    );
    dispMode = "MELEE";
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
#define I2C_SDA_PIN 8 //Corresponds to GPIO pin connected to OLED display's SDA pin.
#endif

#ifndef I2C_SCL_PIN
#define I2C_SCL_PIN 9 //Corresponds to GPIO pin connected to OLED display's SCL pin.
#endif

#ifndef I2C_BLOCK
#define I2C_BLOCK i2c0 //Set depending on which pair of pins you are using - see below.
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
#define I2C_SPEED 400000 //Common values are 100000 for standard, 400000 for fast and 800000 ludicrous speed.
#endif

#ifndef DISPLAY_SIZE
#define DISPLAY_SIZE OLED_128x64 
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

        //Configure display layout options. Change the strings below to make a selection.
        leftLayout = "circles"; // circles, circlesWASD, squares, squaresWASD, htangl
        centerLayout = "circles"; // circles, circles3Button, squares, squares3Button, htangl
        rightLayout = "circles"; // circles, squares, circles19Button, squares19Button, htangl
        
	    obdSetBackBuffer(&obd, ucBackBuffer);
        //Clear screen and render.
        obdFill(&obd, 0, 1);
}

void loop1() {
    if (backends != nullptr) {
        nunchuk->UpdateInputs(backends[0]->GetInputs());
        busy_wait_us(50);
    }

    //Clear screen but don't send to render yet.
    obdFill(&obd, 0, 0);

    //Set mode string based on input combination.
    if (backends[0]->GetInputs().mod_x && !backends[0]->GetInputs().mod_y && backends[0]->GetInputs().start) {
        if (backends[0]->GetInputs().l) {
            dispMode = "MELEE";
        } else if (backends[0]->GetInputs().left) {
            dispMode = "PM";
        } else if (backends[0]->GetInputs().down) {
            dispMode = "ULT";
        } else if (backends[0]->GetInputs().right) {
            dispMode = "FGC";
        } else if (backends[0]->GetInputs().b) {
            dispMode = "RoA";
        }
    }

    // Write communication backend to OLED starting in the top left.
    char char_dispCommBackend[dispCommBackend.length() + 1];
    strcpy(char_dispCommBackend, dispCommBackend.c_str()); //convert string to char
    obdWriteString(&obd, 0, 0, 0, char_dispCommBackend, FONT_6x8, 0, 0);

    // Write current mode to OLED in the top right. 
    // For the x position of the string, we are subtracting the max position (128 for a 128x64 px display) by the number of characters * the font width in px.
    char char_dispMode[dispMode.length() + 1];
    strcpy(char_dispMode, dispMode.c_str()); //convert string to char
    obdWriteString(&obd, 0, 128-(dispMode.length() * 6), 0, char_dispMode, FONT_6x8, 0, 0);

    // Draw buttons.

    if (leftLayout == "circles")
    {
        obdPreciseEllipse(&obd, 6,  29, 4, 4, 1, backends[0]->GetInputs().l);
        obdPreciseEllipse(&obd, 15, 23, 4, 4, 1, backends[0]->GetInputs().left);
        obdPreciseEllipse(&obd, 25, 22, 4, 4, 1, backends[0]->GetInputs().down);
        obdPreciseEllipse(&obd, 35, 27, 4, 4, 1, backends[0]->GetInputs().right);
        obdPreciseEllipse(&obd, 38, 52, 4, 4, 1, backends[0]->GetInputs().mod_x);
        obdPreciseEllipse(&obd, 46, 58, 4, 4, 1, backends[0]->GetInputs().mod_y);
    } else if (leftLayout == "squares")
    {
        obdRectangle(&obd,3,26,9,32,1, backends[0]->GetInputs().l);
        obdRectangle(&obd,12,20,18,26,1, backends[0]->GetInputs().left);
        obdRectangle(&obd,22,19,28,25,1, backends[0]->GetInputs().down);
        obdRectangle(&obd,32,24,38,30,1, backends[0]->GetInputs().right);
        obdRectangle(&obd,35,49,41,55,1, backends[0]->GetInputs().mod_x);
        obdRectangle(&obd,43,55,49,61,1, backends[0]->GetInputs().mod_y);
    }else if (leftLayout == "circlesWASD")
    {
        obdPreciseEllipse(&obd, 6,  29, 4, 4, 1, backends[0]->GetInputs().l);
        obdPreciseEllipse(&obd, 15, 23, 4, 4, 1, backends[0]->GetInputs().left);
        obdPreciseEllipse(&obd, 25, 22, 4, 4, 1, backends[0]->GetInputs().down);
        obdPreciseEllipse(&obd, 29, 13, 4, 4, 1, backends[0]->GetInputs().up);
        obdPreciseEllipse(&obd, 35, 27, 4, 4, 1, backends[0]->GetInputs().right);
        obdPreciseEllipse(&obd, 38, 52, 4, 4, 1, backends[0]->GetInputs().mod_x);
        obdPreciseEllipse(&obd, 46, 58, 4, 4, 1, backends[0]->GetInputs().mod_y);
    }else if (leftLayout == "squaresWASD")
    {
        obdRectangle(&obd,3,26,9,32,1, backends[0]->GetInputs().l);
        obdRectangle(&obd,12,20,18,26,1, backends[0]->GetInputs().left);
        obdRectangle(&obd,22,19,28,25,1, backends[0]->GetInputs().down);
        obdRectangle(&obd,32,24,38,30,1, backends[0]->GetInputs().right);
        obdRectangle(&obd,26,10,32,16,1, backends[0]->GetInputs().up);
        obdRectangle(&obd,35,49,41,55,1, backends[0]->GetInputs().mod_x);
        obdRectangle(&obd,43,55,49,61,1, backends[0]->GetInputs().mod_y);
    }else if (leftLayout == "htangl"){
        obdRectangle(&obd,3,26,9,32,1, backends[0]->GetInputs().l);
        obdRectangle(&obd,12,20,18,26,1, backends[0]->GetInputs().left);
        obdRectangle(&obd,22,19,28,25,1, backends[0]->GetInputs().down);
        obdRectangle(&obd,32,24,38,30,1, backends[0]->GetInputs().right);
        obdRectangle(&obd,35,49,41,55,1, backends[0]->GetInputs().mod_x);
        obdRectangle(&obd,41,55,47,61,1, backends[0]->GetInputs().mod_y);
    };
    
    if (centerLayout == "circles")
    {
        obdPreciseEllipse(&obd, 64, 27, 4, 4, 1, backends[0]->GetInputs().start);
    }else if (centerLayout == "circles3Button")
    {
        obdPreciseEllipse(&obd, 64, 27, 4, 4, 1, backends[0]->GetInputs().start);
        obdPreciseEllipse(&obd, 54, 27, 4, 4, 1, backends[0]->GetInputs().select);
        obdPreciseEllipse(&obd, 74, 27, 4, 4, 1, backends[0]->GetInputs().home);
    }else if (centerLayout == "squares")
    {
        obdRectangle(&obd,61,24,67,30,1, backends[0]->GetInputs().start);
    }else if (centerLayout == "squares3Button")
    {
        obdRectangle(&obd,61,24,67,30,1, backends[0]->GetInputs().start);
        obdRectangle(&obd,51,24,57,30,1, backends[0]->GetInputs().select);
        obdRectangle(&obd,71,24,77,30,1, backends[0]->GetInputs().home);
    }else if (centerLayout == "htangl"){
        obdRectangle(&obd,50,32,56,38,1, backends[0]->GetInputs().select);
        obdRectangle(&obd,61,32,67,38,1, backends[0]->GetInputs().start);
        obdRectangle(&obd,72,32,78,38,1, backends[0]->GetInputs().home);
    };
    
    if (rightLayout == "circles")
    {
        obdPreciseEllipse(&obd, 82,  46, 4, 4, 1, backends[0]->GetInputs().c_left);
        obdPreciseEllipse(&obd, 82,  58, 4, 4, 1, backends[0]->GetInputs().c_down);
        obdPreciseEllipse(&obd, 90,  40, 4, 4, 1, backends[0]->GetInputs().c_up);
        obdPreciseEllipse(&obd, 90,  52, 4, 4, 1, backends[0]->GetInputs().a);
        obdPreciseEllipse(&obd, 98,  46, 4, 4, 1, backends[0]->GetInputs().c_right);
        obdPreciseEllipse(&obd, 93, 17, 4, 4, 1, backends[0]->GetInputs().r);
        obdPreciseEllipse(&obd, 93,  27, 4, 4, 1, backends[0]->GetInputs().b);
        obdPreciseEllipse(&obd, 103, 13, 4, 4, 1, backends[0]->GetInputs().y);
        obdPreciseEllipse(&obd, 102, 23, 4, 4, 1, backends[0]->GetInputs().x);
        obdPreciseEllipse(&obd, 113, 14, 4, 4, 1, backends[0]->GetInputs().lightshield);
        obdPreciseEllipse(&obd, 112, 24, 4, 4, 1, backends[0]->GetInputs().z);
        obdPreciseEllipse(&obd, 122, 19, 4, 4, 1, backends[0]->GetInputs().midshield);
        obdPreciseEllipse(&obd, 122, 29, 4, 4, 1, backends[0]->GetInputs().up);
    }else if (rightLayout == "squares")
    {
        obdRectangle(&obd,79,43,85,49,1, backends[0]->GetInputs().c_left);
        obdRectangle(&obd,79,55,85,61,1, backends[0]->GetInputs().c_down);
        obdRectangle(&obd,87,37,93,43,1, backends[0]->GetInputs().c_up);
        obdRectangle(&obd,87,49,93,55,1, backends[0]->GetInputs().a);
        obdRectangle(&obd,95,43,101,49,1, backends[0]->GetInputs().c_right);
        obdRectangle(&obd,90,14,96,20,1, backends[0]->GetInputs().r);
        obdRectangle(&obd,90,24,96,30,1, backends[0]->GetInputs().b);
        obdRectangle(&obd,100,10,106,16,1, backends[0]->GetInputs().y);
        obdRectangle(&obd,99,20,105,26,1, backends[0]->GetInputs().x);
        obdRectangle(&obd,110,11,116,17,1, backends[0]->GetInputs().lightshield);
        obdRectangle(&obd,109,21,115,27,1, backends[0]->GetInputs().z);
        obdRectangle(&obd,119,16,125,22,1, backends[0]->GetInputs().midshield);
        obdRectangle(&obd,119,26,125,32,1, backends[0]->GetInputs().up);
    }else if (rightLayout == "circles19Button")
    {
        obdPreciseEllipse(&obd, 82,  46, 4, 4, 1, backends[0]->GetInputs().c_left);
        obdPreciseEllipse(&obd, 82,  58, 4, 4, 1, backends[0]->GetInputs().c_down);
        obdPreciseEllipse(&obd, 90,  40, 4, 4, 1, backends[0]->GetInputs().c_up);
        obdPreciseEllipse(&obd, 90,  52, 4, 4, 1, backends[0]->GetInputs().a);
        obdPreciseEllipse(&obd, 98,  46, 4, 4, 1, backends[0]->GetInputs().c_right);
        obdPreciseEllipse(&obd, 93, 17, 4, 4, 1, backends[0]->GetInputs().r);
        obdPreciseEllipse(&obd, 93,  27, 4, 4, 1, backends[0]->GetInputs().b);
        obdPreciseEllipse(&obd, 103, 13, 4, 4, 1, backends[0]->GetInputs().y);
        obdPreciseEllipse(&obd, 102, 23, 4, 4, 1, backends[0]->GetInputs().x);
        obdPreciseEllipse(&obd, 113, 14, 4, 4, 1, backends[0]->GetInputs().lightshield);
        obdPreciseEllipse(&obd, 112, 24, 4, 4, 1, backends[0]->GetInputs().z);
        obdPreciseEllipse(&obd, 122, 29, 4, 4, 1, backends[0]->GetInputs().up);
    }else if (rightLayout == "squares19Button")
    {
        obdRectangle(&obd,79,43,85,49,1, backends[0]->GetInputs().c_left);
        obdRectangle(&obd,79,55,85,61,1, backends[0]->GetInputs().c_down);
        obdRectangle(&obd,87,37,93,43,1, backends[0]->GetInputs().c_up);
        obdRectangle(&obd,87,49,93,55,1, backends[0]->GetInputs().a);
        obdRectangle(&obd,95,43,101,49,1, backends[0]->GetInputs().c_right);
        obdRectangle(&obd,90,14,96,20,1, backends[0]->GetInputs().r);
        obdRectangle(&obd,90,24,96,30,1, backends[0]->GetInputs().b);
        obdRectangle(&obd,100,10,106,16,1, backends[0]->GetInputs().y);
        obdRectangle(&obd,99,20,105,26,1, backends[0]->GetInputs().x);
        obdRectangle(&obd,110,11,116,17,1, backends[0]->GetInputs().lightshield);
        obdRectangle(&obd,109,21,115,27,1, backends[0]->GetInputs().z);
        obdRectangle(&obd,119,26,125,32,1, backends[0]->GetInputs().up);
    }else if (rightLayout == "htangl"){
        obdRectangle(&obd,89,23,95,29,1, backends[0]->GetInputs().b);
        obdRectangle(&obd,99,18,105,24,1, backends[0]->GetInputs().x);
        obdRectangle(&obd,109,19,115,25,1, backends[0]->GetInputs().z);
        obdRectangle(&obd,119,26,125,32,1, backends[0]->GetInputs().up);
        obdRectangle(&obd,89,31,95,37,1, backends[0]->GetInputs().r);
        obdRectangle(&obd,99,26,105,32,1, backends[0]->GetInputs().y);
        obdRectangle(&obd,109,27,115,33,1, backends[0]->GetInputs().lightshield);
        obdRectangle(&obd,119,34,125,40,1, backends[0]->GetInputs().midshield);
        obdRectangle(&obd,88,40,94,46,1, backends[0]->GetInputs().c_up);
        obdRectangle(&obd,80,45,86,51,1, backends[0]->GetInputs().c_left);
        obdRectangle(&obd,80,55,86,61,1, backends[0]->GetInputs().c_down);
        obdRectangle(&obd,88,49,94,55,1, backends[0]->GetInputs().a);
        obdRectangle(&obd,96,45,102,51,1, backends[0]->GetInputs().c_right);
    };

    obdDumpBuffer(&obd,NULL); //Sends buffered content to display.
}