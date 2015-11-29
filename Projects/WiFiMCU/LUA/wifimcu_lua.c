
#include "MiCO.h" 
#include "mico_system.h"
#include "time.h" 
#include "platform_config.h"
#include "CheckSumUtils.h"
#include "lua.h"

extern platform_uart_driver_t platform_uart_drivers[];
extern const platform_uart_t  platform_uart_peripherals[];

#define DEFAULT_WATCHDOG_TIMEOUT        10*1000  // 10 seconds
#define main_log(M, ...) custom_log("main", M, ##__VA_ARGS__)

#define LUA_PARAMS_ID  0xA7
#define LUA_UART        (MICO_UART_1)
#define INBUF_SIZE      256
#define OUTBUF_SIZE     1024

lua_system_param_t lua_system_param =
{
  .ID = LUA_PARAMS_ID,
  .soft_wdg   = 0,
  .wdg_tmo    = DEFAULT_WATCHDOG_TIMEOUT,
  .stack_size = 20*1024,
  .inbuf_size = INBUF_SIZE,
  .baud_rate  = 115200,
  .parity     = NO_PARITY,
  .init_file  = "",
  .crc = 0
};

static uint32_t soft_wdg = 0;
static mico_timer_t _soft_watchdog_timer;
//static uint16_t InterruptDisabledCount = 0;


/*
//--------------------------------
void LUA_DisableInterrupts(void) {
  // Disable interrupts
  __disable_irq();
  // Increase number of disable interrupt function calls
  InterruptDisabledCount++;
}

//----------------------------------
uint8_t LUA_EnableInterrupts(void) {
  // Decrease number of disable interrupt function calls
  if (InterruptDisabledCount) {
          InterruptDisabledCount--;
  }
  // Check if we are ready to enable interrupts
  if (!InterruptDisabledCount) {
          // Enable interrupts
          __enable_irq();
  }
  // Return interrupt enabled status
  return !InterruptDisabledCount;
}
*/

//----------------------------------
uint16_t _get_luaparamsCRC( void ) {
  CRC16_Context paramcrc;
  uint16_t crc = 0;
  uint8_t *p_id = &lua_system_param.ID;

  CRC16_Init( &paramcrc );
  CRC16_Update( &paramcrc, p_id, sizeof(lua_system_param_t)-sizeof(uint16_t) );
  CRC16_Final( &paramcrc, &crc );
  return crc;
}
//---------------------------------------------------
static void _soft_watchdog_timer_handler( void* arg )
{
  (void)(arg);
  soft_wdg += lua_system_param.wdg_tmo / 100; // +100 msec
  if (soft_wdg > lua_system_param.wdg_tmo) {
    MicoSystemReboot();
  }
}

//-------------------------
void luaWdgReload( void ) {
  if (lua_system_param.soft_wdg == 0) MicoWdgReload();
  else soft_wdg = 0;
}

static uint8_t *lua_rx_data;
static ring_buffer_t lua_rx_buffer;
static mico_uart_config_t lua_uart_config =
{
  .baud_rate    = STDIO_UART_BAUDRATE,
  .data_width   = DATA_WIDTH_8BIT,
  .parity       = NO_PARITY,
  .stop_bits    = STOP_BITS_1,
  .flow_control = FLOW_CONTROL_DISABLED,
  .flags        = UART_WAKEUP_DISABLE,
};


//-----------------------------
int lua_putstr(const char *msg)
{
  if (msg[0] != 0)
    MicoUartSend( LUA_UART, (const char*)msg, strlen(msg) );
  return 0;
}

//----------------------------------
int lua_printf(const char *msg, ...)
{
  va_list ap; 
  char *pos, message[256]; 
  int sz; 
  int nMessageLen = 0;
  
  memset(message, 0, 256);
  pos = message;
  
  sz = 0;
  va_start(ap, msg);
  nMessageLen = vsnprintf(pos, 256 - sz, msg, ap);
  va_end(ap);
  
  if( nMessageLen<=0 ) return 0;
  
  lua_putstr((const char*)message);
  return 0;
}

//==========================
int lua_getchar(char *inbuf)
{
  if (MicoUartRecv(LUA_UART, inbuf, 1, 20) == 0)
    return 1;  //OK
  else
    return 0;  //err
}

//=================================================================
int readline4lua(const char *prompt, char *buffer, int buffer_size)
{
    char ch;
    int line_position;
    //lua_printf("\r"); //doit
    
start:
    lua_printf(prompt); // show prompt
    line_position = 0;
    memset(buffer, 0, buffer_size);
    while (1)
    {
      while (lua_getchar(&ch) == 1) {
        if (ch == '\r') {
        // CR key
          char next;
          if (lua_getchar(&next)== 1) ch = next;
        }
        else if (ch == 0x7f || ch == 0x08) {
        // backspace key
          if (line_position > 0) {
            lua_printf("%c %c", ch, ch);
            line_position--;
          }
          buffer[line_position] = 0;
          continue;
        }
        else if (ch == 0x04) {
        // EOF(ctrl+d)
          if (line_position == 0) return 0; // No input which makes lua interpreter close
          else continue;
        }            
        if (ch == '\r' || ch == '\n') {
        // end of line
          buffer[line_position] = 0;
          lua_printf("\r\n"); //doit
          if (line_position == 0) goto start; // Get a empty line, then go to get a new line
          else {
            buffer[line_position+1] = 0;
            //lua_printf("[%s][%d]\r\n", buffer, line_position);
            return line_position;
          }
        }
        if (ch < 0x20 || ch >= 0x80) continue; // other control character or not an acsii character

        lua_printf("%c", ch);       // character echo
        buffer[line_position] = ch; // put received character in buffer
        ch = 0;
        line_position++;
        if (line_position >= buffer_size) {
          // it's a large line, discard it
          goto start;
        }
     }
     // nothing is received
     luaWdgReload();
    }    
}

//=====================================
static void lua_main_thread(void *data)
{
  //lua setup  
  char *argv[] = {"lua", NULL};
  lua_main(1, argv);
  
  //if error happened  
  lua_printf("lua exited, reboot\r\n");
  
  if (lua_system_param.soft_wdg != 0) {
    MicoWdgInitialize( lua_system_param.wdg_tmo);
  }
  mico_thread_msleep(500);
  free(lua_rx_data);
  lua_rx_data = NULL;
  MicoSystemReboot();

  mico_rtos_delete_thread(NULL);
}

//===========================
int application_start( void )
{
//start
  struct tm currentTime;
  uint32_t lua_param_offset = 0x0;
  uint16_t crc = 0;
  uint8_t *p_id = &lua_system_param.ID;
  char *p_f = &lua_system_param.init_file[0];
  mico_rtc_time_t ttime;
  uint8_t prmstat = 0;
  
  platform_check_bootreason();
  MicoInit();

  // Read parameters from flash
  lua_param_offset = 0;
  MicoFlashRead(MICO_PARTITION_PARAMETER_1, &lua_param_offset , p_id, sizeof(lua_system_param_t));
  crc = _get_luaparamsCRC();

  if (lua_system_param.ID != LUA_PARAMS_ID || crc != lua_system_param.crc)  {
    lua_system_param.ID = LUA_PARAMS_ID;
    lua_system_param.soft_wdg = 0;
    lua_system_param.wdg_tmo    = DEFAULT_WATCHDOG_TIMEOUT;
    lua_system_param.stack_size = 20*1024;
    lua_system_param.inbuf_size = INBUF_SIZE;
    lua_system_param.baud_rate = 115200;
    lua_system_param.parity = NO_PARITY;
    sprintf(p_f,"");
    lua_system_param.crc = _get_luaparamsCRC();
    MicoFlashErase(MICO_PARTITION_PARAMETER_1, 0, sizeof(lua_system_param_t));
    lua_param_offset = 0;
    MicoFlashWrite(MICO_PARTITION_PARAMETER_1, &lua_param_offset, p_id, sizeof(lua_system_param_t));
    
  }
  else {
    prmstat = 1;
  }

  //usrinterface
  lua_rx_data = (uint8_t*)malloc(lua_system_param.inbuf_size);
  ring_buffer_init( (ring_buffer_t*)&lua_rx_buffer, (uint8_t*)lua_rx_data, lua_system_param.inbuf_size );
  lua_uart_config.baud_rate = lua_system_param.baud_rate;
  lua_uart_config.parity = (platform_uart_parity_t)lua_system_param.parity;
  
  MicoUartFinalize( STDIO_UART );
  //MicoUartInitialize( STDIO_UART, &lua_uart_config, (ring_buffer_t*)&lua_rx_buffer );
  platform_uart_init( &platform_uart_drivers[LUA_UART], &platform_uart_peripherals[LUA_UART], &lua_uart_config, (ring_buffer_t*)&lua_rx_buffer );

  lua_printf( "\r\n\r\nWiFiMCU Lua starting...(Free memory %d bytes)\r\n",MicoGetMemoryInfo()->free_memory);
  if (prmstat) lua_printf( "Lua params OK\r\n");
  else lua_printf( "BAD Lua params, initialized\r\n");
  if (lua_system_param.soft_wdg==0) lua_printf( "Watchdog: hardware (IWDT).\r\n");
  else lua_printf( "Watchdog: software (timer).\r\n");
  
  if( MicoRtcGetTime(&ttime) == kNoErr ){
    currentTime.tm_sec = ttime.sec;
    currentTime.tm_min = ttime.min;
    currentTime.tm_hour = ttime.hr;
    currentTime.tm_mday = ttime.date;
    currentTime.tm_wday = ttime.weekday;
    currentTime.tm_mon = ttime.month - 1;
    currentTime.tm_year = ttime.year + 100; 
    //lua_printf( "Current Time: %d:%d:%d\r\n",ttime.hr,ttime.min,ttime.sec);
    lua_printf("Current Time: %s",asctime(&currentTime)); 
  }else {
    lua_printf("RTC function unsupported\r\n"); 
  }  
  
  //---watch dog----------
  if (lua_system_param.soft_wdg == 0) {
    MicoWdgInitialize( lua_system_param.wdg_tmo);
  }
  else {
    // soft watchdog timer, 100 msec
    mico_init_timer(&_soft_watchdog_timer,lua_system_param.wdg_tmo/100, _soft_watchdog_timer_handler, NULL);
    mico_start_timer(&_soft_watchdog_timer);
  }
  
  mico_rtos_create_thread(NULL, MICO_DEFAULT_WORKER_PRIORITY, "lua_main_thread", lua_main_thread, lua_system_param.stack_size, 0);

  mico_rtos_delete_thread(NULL);
  lua_printf("application_start exit\r\n");
  return 0;
 }