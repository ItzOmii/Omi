/*
 * PS/2 Keyboard and Mouse Driver
 * For x86_64 MeepOS Desktop Environment
 */

#include "../include/types.h"
#include "../include/gui.h"   /* serial_puts: a driver that silently degrades is undebuggable */

/* ===================================================================== */
/* PS/2 Controller Ports                                                 */
/* ===================================================================== */

#define PS2_DATA_PORT 0x60
#define PS2_STATUS_PORT 0x64
#define PS2_COMMAND_PORT 0x64

/* Status register bits */
#define PS2_STATUS_OUTPUT_FULL 0x01
#define PS2_STATUS_INPUT_FULL 0x02
#define PS2_STATUS_MOUSE_DATA 0x20

/* Controller commands */
#define PS2_CMD_READ_CONFIG 0x20
#define PS2_CMD_WRITE_CONFIG 0x60
#define PS2_CMD_DISABLE_MOUSE 0xA7
#define PS2_CMD_ENABLE_MOUSE 0xA8
#define PS2_CMD_TEST_MOUSE 0xA9
#define PS2_CMD_TEST_CONTROLLER 0xAA
#define PS2_CMD_TEST_KEYBOARD 0xAB
#define PS2_CMD_DISABLE_KB 0xAD
#define PS2_CMD_ENABLE_KB 0xAE
#define PS2_CMD_WRITE_MOUSE 0xD4

/* Keyboard commands (sent to data port) */
#define KB_CMD_SET_LEDS 0xED
#define KB_CMD_ECHO 0xEE
#define KB_CMD_SCANCODE_SET 0xF0
#define KB_CMD_IDENTIFY 0xF2
#define KB_CMD_SET_RATE 0xF3
#define KB_CMD_ENABLE 0xF4
#define KB_CMD_DISABLE 0xF5
#define KB_CMD_DEFAULTS 0xF6
#define KB_CMD_RESET 0xFF

/* Mouse commands (sent via PS2_CMD_WRITE_MOUSE) */
#define MOUSE_CMD_RESET 0xFF
#define MOUSE_CMD_DEFAULTS 0xF6
#define MOUSE_CMD_ENABLE 0xF4
#define MOUSE_CMD_SET_RATE 0xF3
#define MOUSE_CMD_GETID    0xF2

/* ===================================================================== */
/* Port I/O                                                              */
/* ===================================================================== */

static inline uint8_t inb(uint16_t port) {
  uint8_t ret;
  __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
  __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void io_wait(void) {
  /* Small delay for I/O */
  __asm__ volatile("outb %%al, $0x80" : : "a"(0));
}

/* ===================================================================== */
/* Keyboard State                                                        */
/* ===================================================================== */

static volatile int cad_flag;          /* Ctrl+Alt+Del latched, cleared by the shell */
static int shift_pressed = 0;
static int ctrl_pressed = 0;
static int alt_pressed = 0;
static int caps_lock = 0;

/* Keyboard callback */
typedef void (*keyboard_callback_t)(int key);
static keyboard_callback_t key_callback = 0;

/* ===================================================================== */
/* Scancode Set 1 to ASCII Table (US QWERTY)                             */
/* This is the standard PS/2 scancode set 1 (XT) mapping                 */
/* ===================================================================== */

static const char scancode_to_ascii[128] = {
    0,    27,   '1',  '2',  '3',  '4',  '5',  '6',  /* 00-07 */
    '7',  '8',  '9',  '0',  '-',  '=',  '\b', '\t', /* 08-0F */
    'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',  /* 10-17 */
    'o',  'p',  '[',  ']',  '\n', 0,    'a',  's',  /* 18-1F */
    'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',  /* 20-27 */
    '\'', '`',  0,    '\\', 'z',  'x',  'c',  'v',  /* 28-2F */
    'b',  'n',  'm',  ',',  '.',  '/',  0,    '*',  /* 30-37 */
    0,    ' ',  0,    0,    0,    0,    0,    0,    /* 38-3F */
    0,    0,    0,    0,    0,    0,    0,    '7',  /* 40-47 */
    '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',  /* 48-4F */
    '2',  '3',  '0',  '.',  0,    0,    0,    0,    /* 50-57 */
    0,    0,    0,    0,    0,    0,    0,    0,    /* 58-5F */
    0,    0,    0,    0,    0,    0,    0,    0,    /* 60-67 */
    0,    0,    0,    0,    0,    0,    0,    0,    /* 68-6F */
    0,    0,    0,    0,    0,    0,    0,    0,    /* 70-77 */
    0,    0,    0,    0,    0,    0,    0,    0     /* 78-7F */
};

static const char scancode_to_ascii_shift[128] = {
    0,    27,   '!',  '@',  '#',  '$',  '%',  '^',  /* 00-07 */
    '&',  '*',  '(',  ')',  '_',  '+',  '\b', '\t', /* 08-0F */
    'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',  /* 10-17 */
    'O',  'P',  '{',  '}',  '\n', 0,    'A',  'S',  /* 18-1F */
    'D',  'F',  'G',  'H',  'J',  'K',  'L',  ':',  /* 20-27 */
    '"',  '~',  0,    '|',  'Z',  'X',  'C',  'V',  /* 28-2F */
    'B',  'N',  'M',  '<',  '>',  '?',  0,    '*',  /* 30-37 */
    0,    ' ',  0,    0,    0,    0,    0,    0,    /* 38-3F */
    0,    0,    0,    0,    0,    0,    0,    '7',  /* 40-47 */
    '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',  /* 48-4F */
    '2',  '3',  '0',  '.',  0,    0,    0,    0,    /* 50-57 */
    0,    0,    0,    0,    0,    0,    0,    0,    /* 58-5F */
    0,    0,    0,    0,    0,    0,    0,    0,    /* 60-67 */
    0,    0,    0,    0,    0,    0,    0,    0,    /* 68-6F */
    0,    0,    0,    0,    0,    0,    0,    0,    /* 70-77 */
    0,    0,    0,    0,    0,    0,    0,    0     /* 78-7F */
};

/* Special key codes */
#define KEY_UP 0x100
#define KEY_HOME 0x114
#define KEY_END  0x115
#define KEY_PGUP 0x116
#define KEY_PGDN 0x117
#define KEY_DOWN 0x101
#define KEY_LEFT 0x102
#define KEY_RIGHT 0x103
#define KEY_CTRL 0x104
#define KEY_SHIFT 0x105
#define KEY_ALT 0x106
#define KEY_ESC 0x107
#define KEY_F1 0x108
#define KEY_F2 0x109
#define KEY_F3 0x10A
#define KEY_F4 0x10B
#define KEY_F5 0x10C
#define KEY_F6 0x10D
#define KEY_F7 0x10E
#define KEY_F8 0x10F
#define KEY_F9 0x110
#define KEY_F10 0x111
#define KEY_F11 0x112
#define KEY_F12 0x113

/* ===================================================================== */
/* Mouse State                                                           */
/* ===================================================================== */

/* Mouse state - exported for compositor */
volatile int ps2_mouse_x = 400;
volatile int ps2_mouse_y = 300;
volatile int ps2_mouse_buttons = 0;

/* Mouse packet buffer */
static uint8_t mouse_packet[4];
static int mouse_packet_index = 0;
/* Wheel mice report a fourth byte. The device only sends it after the
 * sample-rate knock below has put it in IntelliMouse mode; a plain three-byte
 * mouse would treat that byte as the start of the next packet, so the packet
 * length is decided once, at init, from the device ID. */
static int mouse_packet_len = 3;
static volatile int mouse_wheel;   /* z movement not yet consumed by a poll */

/* Extended scancode state */
static int extended_scancode = 0;

/* Screen bounds for mouse */
static int mouse_max_x = 1920;
static int mouse_max_y = 1080;
static int mouse_scale = 2;

/* ===================================================================== */
/* PS/2 Controller Helper Functions                                      */
/* ===================================================================== */

static void ps2_wait_input(void) {
  int timeout = 100000;
  while ((inb(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL) && timeout > 0) {
    timeout--;
    io_wait();
  }
}

static void ps2_wait_output(void) {
  int timeout = 100000;
  while (!(inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) && timeout > 0) {
    timeout--;
    io_wait();
  }
}

static void ps2_send_command(uint8_t cmd) {
  ps2_wait_input();
  outb(PS2_COMMAND_PORT, cmd);
}

static void ps2_send_data(uint8_t data) {
  ps2_wait_input();
  outb(PS2_DATA_PORT, data);
}

static uint8_t ps2_read_data(void) {
  ps2_wait_output();
  return inb(PS2_DATA_PORT);
}

static void ps2_flush_output(void) {
  int timeout = 1000;
  while ((inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) && timeout > 0) {
    inb(PS2_DATA_PORT);
    io_wait();
    timeout--;
  }
}

static int ps2_keyboard_send(uint8_t cmd) {
  int retries = 3;
  while (retries-- > 0) {
    ps2_wait_input();
    outb(PS2_DATA_PORT, cmd);
    
    ps2_wait_output();
    uint8_t resp = inb(PS2_DATA_PORT);
    
    if (resp == 0xFA) return 0;  /* ACK */
    if (resp == 0xFE) continue;  /* Resend */
  }
  return -1;
}

static void ps2_mouse_write(uint8_t data) {
  ps2_send_command(PS2_CMD_WRITE_MOUSE);
  ps2_send_data(data);
}

/* ===================================================================== */
/* PS/2 Polling                                                          */
/* ===================================================================== */

void ps2_poll(void) {
  /* Process all available data - aggressive polling for USB legacy emulation */
  for (int i = 0; i < 64; i++) {
    /* Small delay between status checks - helps with USB legacy emulation */
    for (volatile int d = 0; d < 10; d++) {
      __asm__ volatile("pause");
    }
    
    uint8_t status = inb(PS2_STATUS_PORT);
    
    if (!(status & PS2_STATUS_OUTPUT_FULL)) {
      break;  /* No more data */
    }
    
    uint8_t data = inb(PS2_DATA_PORT);
    
    if (status & PS2_STATUS_MOUSE_DATA) {
      /* ============ MOUSE DATA ============ */
      mouse_packet[mouse_packet_index++] = data;
      
      /* First byte must have bit 3 set */
      if (mouse_packet_index == 1 && !(data & 0x08)) {
        mouse_packet_index = 0;
        continue;
      }
      
      if (mouse_packet_index >= mouse_packet_len) {
        mouse_packet_index = 0;
        
        uint8_t flags = mouse_packet[0];
        int dx = mouse_packet[1];
        int dy = mouse_packet[2];

        /* Wheel: a signed four-bit nibble in the fourth byte, positive for
         * away from the user. Accumulated rather than overwritten, because a
         * fast flick can deliver several packets between two polls. */
        if (mouse_packet_len == 4) {
          int z = mouse_packet[3] & 0x0F;
          if (z & 0x08) z -= 16;
          mouse_wheel += z;
        }
        
        /* Sign extension */
        if (flags & 0x10) dx -= 256;
        if (flags & 0x20) dy -= 256;
        
        /* Handle overflow - discard */
        if (flags & 0x40) dx = 0;
        if (flags & 0x80) dy = 0;
        
        /* Sensitivity */
        dx *= mouse_scale;
        dy *= mouse_scale;
        
        /* Update position */
        ps2_mouse_x += dx;
        ps2_mouse_y -= dy;
        
        /* Clamp */
        if (ps2_mouse_x < 0) ps2_mouse_x = 0;
        if (ps2_mouse_x >= mouse_max_x) ps2_mouse_x = mouse_max_x - 1;
        if (ps2_mouse_y < 0) ps2_mouse_y = 0;
        if (ps2_mouse_y >= mouse_max_y) ps2_mouse_y = mouse_max_y - 1;
        
        /* Buttons */
        ps2_mouse_buttons = 0;
        if (flags & 0x01) ps2_mouse_buttons |= 1;
        if (flags & 0x02) ps2_mouse_buttons |= 2;
        if (flags & 0x04) ps2_mouse_buttons |= 4;
      }
    } else {
      /* ============ KEYBOARD DATA ============ */
      uint8_t scancode = data;
      int key = 0;
      
      /* Handle extended scancode prefix (0xE0) */
      if (scancode == 0xE0) {
        extended_scancode = 1;
        continue;
      }
      
      /* Handle extended scancode prefix (0xE1 for Pause) */
      if (scancode == 0xE1) {
        extended_scancode = 2;
        continue;
      }
      
      /* Skip remaining bytes of E1 sequence */
      if (extended_scancode == 2) {
        extended_scancode--;
        continue;
      }
      
      /* Key release (high bit set) */
      if (scancode & 0x80) {
        uint8_t released = scancode & 0x7F;
        if (extended_scancode) {
          extended_scancode = 0;
          /* Extended key release - handle right ctrl/alt */
          switch (released) {
          case 0x1D: ctrl_pressed = 0; break;   /* Right Ctrl */
          case 0x38: alt_pressed = 0; break;    /* Right Alt */
          }
        } else {
          switch (released) {
          case 0x2A: case 0x36: shift_pressed = 0; break;  /* Shift */
          case 0x1D: ctrl_pressed = 0; break;              /* Ctrl */
          case 0x38: alt_pressed = 0; break;               /* Alt */
          }
        }
        continue;
      }
      
      /* Handle extended key press */
      if (extended_scancode) {
        extended_scancode = 0;
        switch (scancode) {
        case 0x48: key = KEY_UP; break;
        case 0x50: key = KEY_DOWN; break;
        case 0x4B: key = KEY_LEFT; break;
        case 0x4D: key = KEY_RIGHT; break;
        case 0x1D: ctrl_pressed = 1; continue;   /* Right Ctrl */
        case 0x38: alt_pressed = 1; continue;    /* Right Alt */
        /* Home/End/Page Up/Page Down get their own codes: folding them into
         * the arrows made a browser that could not jump to either end of a
         * page or scroll a screenful at a time. */
        case 0x47: key = KEY_HOME; break;
        case 0x4F: key = KEY_END;  break;
        case 0x49: key = KEY_PGUP; break;
        case 0x51: key = KEY_PGDN; break;
        case 0x53:                               /* Delete (E0 53) */
          if (ctrl_pressed && alt_pressed) { cad_flag = 1; continue; }
          key = '\b'; break;                     /* plain Delete -> Backspace */
        case 0x1C: key = '\n'; break;            /* Keypad Enter */
        default: continue;
        }
        if (key && key_callback) {
          key_callback(key);
        }
        continue;
      }
      
      /* Key press - handle modifiers */
      switch (scancode) {
      case 0x2A: case 0x36: shift_pressed = 1; continue;  /* Shift */
      case 0x1D: ctrl_pressed = 1; continue;              /* Ctrl */
      case 0x38: alt_pressed = 1; continue;               /* Alt */
      case 0x3A: caps_lock = !caps_lock; continue;        /* Caps Lock */
      }
      
      /* Special keys */
      switch (scancode) {
      case 0x48: key = KEY_UP; break;
      case 0x50: key = KEY_DOWN; break;
      case 0x4B: key = KEY_LEFT; break;
      case 0x4D: key = KEY_RIGHT; break;
      case 0x01: key = KEY_ESC; break;
      case 0x3B: key = KEY_F1; break;
      case 0x3C: key = KEY_F2; break;
      case 0x3D: key = KEY_F3; break;
      case 0x3E: key = KEY_F4; break;
      case 0x3F: key = KEY_F5; break;
      case 0x40: key = KEY_F6; break;
      case 0x41: key = KEY_F7; break;
      case 0x42: key = KEY_F8; break;
      case 0x43: key = KEY_F9; break;
      case 0x44: key = KEY_F10; break;
      case 0x57: key = KEY_F11; break;
      case 0x58: key = KEY_F12; break;
      default:
        /* Regular keys */
        if (scancode < 128) {
          int use_shift = shift_pressed;
          
          /* Caps lock affects letters only */
          char base = scancode_to_ascii[scancode];
          if (caps_lock && base >= 'a' && base <= 'z') {
            use_shift = !use_shift;
          }
          
          if (use_shift) {
            key = scancode_to_ascii_shift[scancode];
          } else {
            key = scancode_to_ascii[scancode];
          }
        }
        break;
      }
      
      /* Call callback if we have a valid key */
      if (key && key_callback) {
        key_callback(key);
      }
    }
  }
}

/* ===================================================================== */
/* PS/2 Initialization                                                   */
/* ===================================================================== */

void ps2_set_keyboard_callback(keyboard_callback_t callback) {
  key_callback = callback;
}

void ps2_set_screen_bounds(int width, int height) {
  mouse_max_x = width;
  mouse_max_y = height;
  ps2_mouse_x = width / 2;
  ps2_mouse_y = height / 2;
}

void ps2_set_mouse_scale(int scale) {
  if (scale < 1)
    scale = 1;
  if (scale > 8)
    scale = 8;
  mouse_scale = scale;
}

int ps2_init(void) {
  /* Flush any stale data first */
  for (int i = 0; i < 100; i++) {
    if (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) {
      inb(PS2_DATA_PORT);
      io_wait();
    }
  }
  
  /* Disable both devices */
  ps2_send_command(PS2_CMD_DISABLE_KB);
  io_wait();
  ps2_send_command(PS2_CMD_DISABLE_MOUSE);
  io_wait();
  
  /* Flush any pending data */
  ps2_flush_output();
  
  /* Read and modify config */
  ps2_send_command(PS2_CMD_READ_CONFIG);
  io_wait();
  uint8_t config = ps2_read_data();
  
  /* Disable IRQs initially, enable clock, ENABLE translation (bit 6) */
  /* Translation converts Set 2 scancodes to Set 1 which our tables expect */
  config &= ~0x03;  /* Disable IRQs (bits 0,1) */
  config |= 0x44;   /* Enable translation (bit 6) + System flag (bit 2) */
  config &= ~0x30;  /* Enable clocks (bits 4,5 = 0) */
  
  ps2_send_command(PS2_CMD_WRITE_CONFIG);
  ps2_send_data(config);
  io_wait();
  
  /* Self test - skip if it fails on some hardware */
  ps2_send_command(PS2_CMD_TEST_CONTROLLER);
  io_wait();
  int timeout = 10000;
  while (!(inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) && timeout > 0) {
    io_wait();
    timeout--;
  }
  if (timeout > 0) {
    uint8_t test_result = inb(PS2_DATA_PORT);
    if (test_result != 0x55) {
      /* Self-test failed - continue anyway on some hardware */
    }
  }
  
  /* Re-write config (some controllers reset it after self-test) */
  ps2_send_command(PS2_CMD_WRITE_CONFIG);
  ps2_send_data(config);
  io_wait();
  
  /* Enable both ports */
  ps2_send_command(PS2_CMD_ENABLE_KB);
  io_wait();
  ps2_send_command(PS2_CMD_ENABLE_MOUSE);
  io_wait();
  
  /* Test mouse port */
  ps2_send_command(PS2_CMD_TEST_MOUSE);
  io_wait();
  for (int i = 0; i < 1000; i++) io_wait();
  ps2_flush_output();
  
  /* Initialize keyboard with robust sequence for real hardware */
  ps2_flush_output();
  
  /* Reset keyboard to defaults */
  ps2_keyboard_send(KB_CMD_DEFAULTS);
  for (int i = 0; i < 5000; i++) io_wait();
  ps2_flush_output();
  
  /* Don't force scancode set - let translation handle it */
  /* Many modern laptops don't respond well to scancode set commands */
  
  /* Enable keyboard scanning */
  ps2_keyboard_send(KB_CMD_ENABLE);
  for (int i = 0; i < 5000; i++) io_wait();
  ps2_flush_output();
  
  /* Initialize mouse - with retries for real hardware */
  for (int retry = 0; retry < 3; retry++) {
    ps2_flush_output();
    
    /* Reset mouse */
    ps2_mouse_write(MOUSE_CMD_RESET);
    for (int i = 0; i < 20000; i++) io_wait();  /* Longer wait for real hardware */
    ps2_flush_output();
    
    /* Set defaults */
    ps2_mouse_write(MOUSE_CMD_DEFAULTS);
    for (int i = 0; i < 5000; i++) io_wait();
    ps2_flush_output();
    
    /* Set sample rate to 100 */
    ps2_mouse_write(MOUSE_CMD_SET_RATE);
    for (int i = 0; i < 1000; i++) io_wait();
    ps2_flush_output();
    ps2_mouse_write(100);
    for (int i = 0; i < 1000; i++) io_wait();
    ps2_flush_output();

    /* Ask for the wheel: the IntelliMouse knock is three sample rates in a row
     * (200, 100, 80), after which the device reports a 4-byte packet and answers
     * its ID as 3. A mouse that ignores the knock stays at ID 0 and keeps
     * sending 3-byte packets - which is why the length is set from the ID rather
     * than assumed. Every write's ack is drained before the next, or the ID read
     * below returns a leftover 0xFA and the mouse is misconfigured for good. */
    {
      static const uint8_t knock[3] = { 200, 100, 80 };
      for (int k = 0; k < 3; k++) {
        ps2_mouse_write(MOUSE_CMD_SET_RATE);
        for (int i = 0; i < 500; i++) io_wait();
        ps2_flush_output();
        ps2_mouse_write(knock[k]);
        for (int i = 0; i < 500; i++) io_wait();
        ps2_flush_output();
      }
      ps2_mouse_write(MOUSE_CMD_GETID);
      for (int i = 0; i < 2000; i++) io_wait();
      int id = -1, saw_ack = 0;
      for (int i = 0; i < 4; i++) {
        if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL)) break;
        uint8_t b = inb(PS2_DATA_PORT);
        if (b == 0xFA) { saw_ack = 1; continue; }
        if (saw_ack) { id = b; break; }
      }
      mouse_packet_len = (id == 3 || id == 4) ? 4 : 3;
      serial_puts("[ps2] mouse id=");
      serial_puthex((uint64_t)(id < 0 ? 0xFF : id));
      serial_puts(mouse_packet_len == 4 ? " wheel: 4-byte packets\n" : " no wheel: 3-byte packets\n");
      ps2_flush_output();
    }
    /* Enable data reporting */
    ps2_mouse_write(MOUSE_CMD_ENABLE);
    for (int i = 0; i < 1000; i++) io_wait();
    ps2_flush_output();
    
    /* Check if mouse responds by reading a test packet */
    /* Some mice need a moment to start reporting */
    for (int i = 0; i < 5000; i++) io_wait();
  }
  
  /* Re-read and enable IRQs for both ports */
  ps2_send_command(PS2_CMD_READ_CONFIG);
  io_wait();
  config = ps2_read_data();
  config |= 0x03;  /* Enable IRQs for both keyboard (bit 0) and mouse (bit 1) */
  config &= ~0x30; /* Make sure clocks are enabled (bits 4,5 = 0) */
  ps2_send_command(PS2_CMD_WRITE_CONFIG);
  ps2_send_data(config);
  io_wait();
  
  /* Final flush */
  ps2_flush_output();
  
  return 0;
}

/* ===================================================================== */
/* API Functions                                                         */
/* ===================================================================== */

int ps2_get_mouse_x(void) { return ps2_mouse_x; }
int ps2_get_mouse_y(void) { return ps2_mouse_y; }
int ps2_get_mouse_buttons(void) { return ps2_mouse_buttons; }

/* Wheel movement since the last call, in notches, positive away from the user.
 * Consuming rather than latching means a frame that arrives late still sees
 * every notch once, and none twice. */
int ps2_get_mouse_wheel(void) {
  int v = mouse_wheel;
  mouse_wheel = 0;
  return v;
}

/* Keep old handler functions for compatibility */
void ps2_keyboard_handler(void) { ps2_poll(); }
void ps2_mouse_handler(void) { ps2_poll(); }

/* Ctrl+Alt+Del is a chord, not a character: it is latched here and the shell
 * polls for it, so nothing in the keyboard callback path has to know. */
int ps2_take_ctrl_alt_del(void) {
  int v = cad_flag;
  cad_flag = 0;
  return v;
}
