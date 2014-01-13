/**
 * \file
 * \brief Radio Module
 * \author Erich Styger, erich.styger@hslu.ch
 *
 * Implementation of the Radio module to handle everything around the radio transceiver
 */

#include "Platform.h"
#if PL_HAS_RADIO
#include "SMAC1.h"
#include "UTIL1.h"
#include "Radio.h"
#include "Event.h"
#include "LED.h"
#include "CLS1.h"
#include "FRTOS1.h"
#include "UTIL1.h"
#include "Shell.h"
#if PL_HAS_REMOTE
  #include "Remote.h"
#endif
#if PL_HAS_RTOS_TRACE
  #include "Ptrc1.h"
  #include "ISRtrace.h"
#endif

#if PL_HAS_CONTEST
static bool RADIO_isContest = FALSE;
static bool RADIO_isContestSender = FALSE;
static unsigned char ContestCalcExpectedResultStr[16];
static uint8_t RADIO_contestNo;
#endif

/* Radio state definitions */
typedef enum RADIO_AppStatusKind {
  RADIO_INITIAL_STATE,
  RADIO_RESET_STATE,
  RADIO_RECEIVER_ALWAYS_ON,
  RADIO_TRANSMIT_DATA,
  RADIO_WAITING_FOR_ACK,
  RADIO_TRANSMIT_ACK,
  RADIO_READY_FOR_TX_RX_DATA
} RADIO_AppStatusKind;

#define RADIO_QUEUE_NOF_ITEMS  8 /* number of items in the queue */
#define RADIO_QUEUE_ITEM_SIZE  32 /* size of each queue item */
static xQueueHandle RADIO_MsgQueue; /* queue for messages,  format is: kind(8bit) dataSize(8bit) data */

typedef enum RADIO_QueueMsgKind {
  RADIO_QUEUE_MSG_SNIFF, /* sniffing message */
#if PL_HAS_CONTEST
  RADIO_QUEUE_MSG_CONTEST_QUESTION, /* contest question message */
  RADIO_QUEUE_MSG_CONTEST_ANSWER, /* contest question message */
#endif
  RADIO_QUEUE_MSG_ACCEL  /* acceleration message */
} RADIO_QueueMsgKind;

/*! \todo RADIO: Define here your PAN ID and acknowledge message you want to use */
#define RADIO_PREFIX_STR "EST"   /* prefix used for every message */
#define RADIO_ACK_STR    "ack"   /* acknowledge string */

#define RADIO_TIMEOUT_COUNT    0xB000 /*!< how long the timeout value will be while transmitting a message */

static RADIO_AppStatusKind RADIO_AppStatus = RADIO_INITIAL_STATE;
static uint8_t RADIO_Channel = 5;
static uint8_t RADIO_OutputPower = 15;
static bool RADIO_isOn = TRUE;
static bool RADIO_isSniffing = FALSE;

static tRxPacket RADIO_RxPacket;            /*!< SMAC structure for RX packets */
static uint8_t RADIO_RxDataBuffer[SMAC1_RADIO_BUF_SIZE]; /*!< Data buffer to hold RX data */

static tTxPacket RADIO_TxPacket;            /*!< SMAC structure for TX packets */
static uint8_t RADIO_TxDataBuffer[SMAC1_RADIO_BUF_SIZE]; /*!< Data buffer to hold TX data */

/*!
 \brief A simple state machine iterating through different transceiver states
 */
static void RADIO_HandleState(void) {
  tTxPacket ackTxPacket;  /* SMAC structure for TX packets   */
  static uint8_t ackTxDataBuffer[SMAC1_RADIO_BUF_SIZE];    /*Data buffer to hold TX data */

  switch (RADIO_AppStatus) {
    case RADIO_INITIAL_STATE:
        RADIO_AppStatus = RADIO_RECEIVER_ALWAYS_ON;
        break;

    case RADIO_RECEIVER_ALWAYS_ON:
        RADIO_AppStatus = RADIO_READY_FOR_TX_RX_DATA;
        (void)SMAC1_MLMERXEnableRequest(&RADIO_RxPacket, 0); /* Zero means wait forever with RX ON. */
        break;

    case RADIO_READY_FOR_TX_RX_DATA: /* we are ready to receive/send data data */
        break;

    case RADIO_TRANSMIT_DATA:
        if (SMAC1_MLMERXDisableRequest() != SMAC1_SUCCESS) { /* Turn off the RX forever mode. */
          RADIO_AppStatus = RADIO_TRANSMIT_DATA; /* retry */
          break;
        }
        LED1_Neg();
        if ((SMAC1_MCPSDataRequest(&RADIO_TxPacket) == SMAC1_SUCCESS)) { /* transmit data */
#if PL_HAS_CONTEST
          if (RADIO_isContest) {
            RADIO_AppStatus = RADIO_RECEIVER_ALWAYS_ON;
          } else {
            RADIO_AppStatus = RADIO_WAITING_FOR_ACK;
          }
#else
          RADIO_AppStatus = RADIO_WAITING_FOR_ACK;
#endif
          (void)SMAC1_MLMERXEnableRequest(&RADIO_RxPacket, RADIO_TIMEOUT_COUNT);
        } else {
          RADIO_AppStatus = RADIO_RECEIVER_ALWAYS_ON; /* what should we otherwise do? */
        }
        break;

    case RADIO_TRANSMIT_ACK:
        /*Initialize the packet.*/
        /*! \todo RADIO: Below we send back the acknowledge message:
         *        check that your RADIO_ACK_STR is what you want and need.
         *        Notice the implicit string concatenation for efficiency and reduced code size. */
        UTIL1_strcpy((char*)ackTxDataBuffer, sizeof(ackTxDataBuffer), RADIO_PREFIX_STR RADIO_ACK_STR);
        ackTxPacket.pu8Data = &ackTxDataBuffer[0]; /* Load the address of our txbuffer into the tx structure*/
        ackTxPacket.u8DataLength = (byte)(UTIL1_strlen((char*)ackTxDataBuffer)+1); /* set the size of the packet */
        (void)SMAC1_MCPSDataRequest(&ackTxPacket); /* transmit data */
        RADIO_AppStatus = RADIO_RECEIVER_ALWAYS_ON;
        break;

    case RADIO_RESET_STATE:
        /* MC13192 Reset, reinitialize and return to default state.   */
        SMAC1_RadioInit();
        RADIO_AppStatus = RADIO_INITIAL_STATE;
        break;

    case RADIO_WAITING_FOR_ACK:
        /* At this point only two things happen, 1-we receive the ack packet or 2-timeout.
         * Either way the TX will leave this state and continue. Low power mode could be placed here
         * because both 1 and 2 are interrupt driven, in this case we keep it simple
         */
        break;

    default:
        break;
  }
}

static void QueueMessage(uint8_t kind, const char *msg, uint8_t msgSize) {
  /* format is: kind(8bit) dataSize(8bit) data */
  uint8_t i, buf[RADIO_QUEUE_ITEM_SIZE];
  signed portBASE_TYPE pxHigherPriorityTaskWoken;

  buf[0] = kind;
  buf[1] = msgSize;

  i = 2;
  while(msgSize>0 && i<sizeof(buf)) {
    buf[i++] = *msg;
    msg++;
    msgSize--;
  }
  if (FRTOS1_xQueueSendToBackFromISR(RADIO_MsgQueue, &buf[0], &pxHigherPriorityTaskWoken)!=pdTRUE) {
    /* was not able to send to the queue. Well, not much we can do here... */
  }
}

#if PL_HAS_CONTEST
static bool IsContestMessage(unsigned char *msg, bool *isQuestion, int8_t *ap, int8_t *bp, unsigned char *op) {
  /* must be of format "number op number = ?" or "number op number = number" */
  int32_t val;
  const unsigned char *p;

  *isQuestion = FALSE;
  p = msg;
  if (UTIL1_xatoi(&p, &val)!=ERR_OK) { /* not a number */
    return FALSE;
  }
  *ap = (int8_t)val;
  while(*p==' ' && *p != '\0') { /* skip spaces */
    p++;
  }
  if (!(*p=='+' || *p=='-' || *p=='*' || *p=='/')) { /* must be one of the base operations */
    return FALSE;
  }
  *op = *p;
  p++;
  if (UTIL1_xatoi(&p, &val)!=ERR_OK) { /* not a number */
    return FALSE;
  }
  *bp = (int8_t)val;
  while(*p==' ' && *p != '\0') { /* skip spaces */
    p++;
  }
  if (*p != '=') {
    return FALSE;
  }
  p++;
  while(*p==' ' && *p != '\0') { /* skip spaces */
    p++;
  }
  if (*p=='?') {
    *isQuestion = TRUE;
    return TRUE;
  }
  if (UTIL1_xatoi(&p, &val)!=ERR_OK) { /* not a number */
    return FALSE;
  }
  return TRUE; /* answer message */
}
#endif


void RADIO_DataIndicationPacket(tRxPacket *sRxPacket) {
#if PL_HAS_CONTEST
  bool isQuestion;
  int8_t dummyVal;
  uint8_t dummyOp;
#endif

  if (sRxPacket->u8Status==SMAC1_TIMEOUT) {      /* Put timeout condition code here */
    LED1_Neg(); LED2_Neg();  /* indicator for bad or no communication */
    EVNT_SetEvent(EVNT_RADIO_TIMEOUT);
  } else if (sRxPacket->u8Status == SMAC1_SUCCESS) { /* good packet received: handle it. */
    if (RADIO_isSniffing) {
      QueueMessage(RADIO_QUEUE_MSG_SNIFF, (const char*)sRxPacket->pu8Data, sRxPacket->u8DataLength);
    }
    /* check if it is the packet we expect...*/
    if (   RADIO_AppStatus==RADIO_WAITING_FOR_ACK
        && UTIL1_strncmp((char*)sRxPacket->pu8Data, RADIO_PREFIX_STR RADIO_ACK_STR, sizeof(RADIO_PREFIX_STR RADIO_ACK_STR)-1)==0
       ) /* is it our acknowledge packet? */
    {
      EVNT_SetEvent(EVNT_RADIO_ACK);
#if PL_HAS_CONTEST
    } else if (RADIO_isContest && IsContestMessage(sRxPacket->pu8Data, &isQuestion, &dummyVal, &dummyVal, &dummyOp)) {
      if (isQuestion) {
        QueueMessage(RADIO_QUEUE_MSG_CONTEST_QUESTION, (const char*)sRxPacket->pu8Data, sRxPacket->u8DataLength);
      } else {
        QueueMessage(RADIO_QUEUE_MSG_CONTEST_ANSWER, (const char*)sRxPacket->pu8Data, sRxPacket->u8DataLength);
      }
      EVNT_SetEvent(EVNT_RADIO_DATA);
#endif
    } else if (UTIL1_strncmp((char*)sRxPacket->pu8Data, RADIO_PREFIX_STR, sizeof(RADIO_PREFIX_STR)-1)==0) {
#if PL_HAS_REMOTE && PL_HAS_MOTOR
      /*! \todo Implement your own message handling */
      if (UTIL1_strncmp((char*)sRxPacket->pu8Data, RADIO_PREFIX_STR REMOTE_ACCEL_PREFIX, sizeof(RADIO_PREFIX_STR REMOTE_ACCEL_PREFIX)-1)==0) {
        QueueMessage(RADIO_QUEUE_MSG_ACCEL, (const char*)sRxPacket->pu8Data, sRxPacket->u8DataLength);
      }
#endif
      EVNT_SetEvent(EVNT_RADIO_DATA);
    } else { /* unknown packet? */
      EVNT_SetEvent(EVNT_RADIO_UNKNOWN);
    }
  } else if (sRxPacket->u8Status==SMAC1_OVERFLOW) { /* received packet, but it was longer than what we expect. */
    EVNT_SetEvent(EVNT_RADIO_OVERFLOW);
    LED1_Neg(); LED2_Neg(); /* indicator for bad or no communication */
  }
}

void RADIO_ResetIndication(void) {
  EVNT_SetEvent(EVNT_RADIO_RESET);         /* MC13192 reset, re-initialize.*/
}

void RADIO_AppHandleEvent(EVNT_Handle event) {
  /*! \todo You might disable later on some of the messages. */
  switch(event) {
    case EVNT_RADIO_RESET: /* radio transceiver has reset */
      SHELL_SendMessage("RADIO reset\r\n");
      RADIO_AppStatus = RADIO_RESET_STATE;
      break;
    case EVNT_RADIO_TIMEOUT: /* packet sent was causing timeout */
      SHELL_SendMessage("RADIO timeout\r\n");
      RADIO_AppStatus = RADIO_RECEIVER_ALWAYS_ON;
      break;
    case EVNT_RADIO_ACK: /* acknowledge received */
      SHELL_SendMessage("RADIO rx ack\r\n");
      RADIO_AppStatus = RADIO_RECEIVER_ALWAYS_ON;
      break;
    case EVNT_RADIO_OVERFLOW: /* packet received was too long */
      SHELL_SendMessage("RADIO overflow\r\n");
      RADIO_AppStatus = RADIO_RECEIVER_ALWAYS_ON;
      break;
    case EVNT_RADIO_DATA: /* data received */
      SHELL_SendMessage("RADIO rx data, going to tx ACK\r\n");
      RADIO_AppStatus = RADIO_TRANSMIT_ACK;
      break;
    case EVNT_RADIO_UNKNOWN: /* unknown package received */
      SHELL_SendMessage("RADIO unknown\r\n");
      RADIO_AppStatus = RADIO_RECEIVER_ALWAYS_ON;
      break;
  } /* switch */
}

#if PL_HAS_CONTEST
static void CalcResultString(unsigned char *buf, size_t bufSize, int8_t opA, int8_t opB, unsigned char op) {
  UTIL1_Num8sToStr(buf, bufSize, opA);
  UTIL1_chcat(buf, bufSize, ' ');
  UTIL1_chcat(buf, bufSize, op);
  UTIL1_chcat(buf, bufSize, ' ');
  UTIL1_strcatNum8s(buf, bufSize, opB);
  UTIL1_strcat(buf, bufSize, " = ");
  if (op == '+') {
    UTIL1_strcatNum8s(buf, bufSize, opA+opB);
  } else if (op == '-') {
    UTIL1_strcatNum8s(buf, bufSize, opA-opB);
  } else if (op == '*') {
    UTIL1_strcatNum8s(buf, bufSize, opA*opB);
  } else if (op == '/') {
    if (opB!=0) {
      UTIL1_strcatNum8s(buf, bufSize, opA/opB);
    } else {
      UTIL1_strcat(buf, bufSize, "ERROR");
    }
  }
  UTIL1_strcat(buf, bufSize, " Joe TheBest!");
}

static void RespondWithAnswer(unsigned char *msg) {
  unsigned char buf[36];
  int8_t opA, opB;
  unsigned char op;
  bool isQuestion;

  if (IsContestMessage(msg, &isQuestion, &opA, &opB, &op) && isQuestion) {
    CalcResultString(buf, sizeof(buf), opA, opB, op);
    CLS1_SendStr("Contest answer: ", CLS1_GetStdio()->stdOut);
    CLS1_SendStr(buf, CLS1_GetStdio()->stdOut);
    CLS1_SendStr("\r\n", CLS1_GetStdio()->stdOut);
    RADIO_SendStringRaw(buf);
  }
}
#endif


/*!
 * \brief Sends a data packet trough the Radio
 * \param[in] data Data to be sent (binary), zero terminated
*/
void RADIO_SendString(const char *data) {
  if (!RADIO_isOn) {
    return;
  }
  while (RADIO_AppStatus != RADIO_READY_FOR_TX_RX_DATA) { /* we are not ready yet! */
    RADIO_HandleState(); /* advance state machine */
  }
  UTIL1_strcpy((char*)RADIO_TxDataBuffer, sizeof(RADIO_TxDataBuffer), RADIO_PREFIX_STR);
  UTIL1_strcat((char*)RADIO_TxDataBuffer, sizeof(RADIO_TxDataBuffer), data);
  RADIO_TxPacket.pu8Data = &RADIO_TxDataBuffer[0];            /* Load the address of our txbuffer into tx structure.*/
  RADIO_TxPacket.u8DataLength = (byte)(UTIL1_strlen((char*)RADIO_TxDataBuffer)+1); /* Set the data length of the packet */
  RADIO_AppStatus = RADIO_TRANSMIT_DATA;
  RADIO_HandleState(); /* advance state machine */
}

void RADIO_SendStringRaw(const char *data) {
  if (!RADIO_isOn) {
    return;
  }
  while (RADIO_AppStatus != RADIO_READY_FOR_TX_RX_DATA) { /* we are not ready yet! */
    RADIO_HandleState(); /* advance state machine */
  }
  UTIL1_strcpy((char*)RADIO_TxDataBuffer, sizeof(RADIO_TxDataBuffer), data);
  RADIO_TxPacket.pu8Data = &RADIO_TxDataBuffer[0];            /* Load the address of our txbuffer into tx structure.*/
  RADIO_TxPacket.u8DataLength = (byte)(UTIL1_strlen((char*)RADIO_TxDataBuffer)+1); /* Set the data length of the packet */
  RADIO_AppStatus = RADIO_TRANSMIT_DATA;
  RADIO_HandleState(); /* advance state machine */
}


/*!
 * \brief Sets the channel number to be used
 * \param ch The channel to be used, in the range 0..15
 */
static void RADIO_SetChannel(uint8_t ch) {
  RADIO_Channel = ch&0xF; /* make sure it remains in range 0..15 */
  (void)SMAC1_MLMESetChannelRequest(RADIO_Channel);  /* Set channel */
}

/*!
 * \brief Sets the channel number to be used
 * \param power The channel to be used, in the range 0..15
 */
static void RADIO_SetOutputPower(uint8_t power) {
  RADIO_OutputPower = power&0xF; /* make sure it remains in range 0..15 */
  (void)SMAC1_MLMEMC13192PAOutputAdjust(RADIO_OutputPower); /* Set output power setting */
}

static void RADIO_PrintHelp(const CLS1_StdIOType *io) {
  CLS1_SendHelpStr("radio", "Group of radio commands\r\n", io->stdOut);
  CLS1_SendHelpStr("  help|status", "Shows radio help or status\r\n", io->stdOut);
  CLS1_SendHelpStr("  on|off", "Turns the radio on or off\r\n", io->stdOut);
  CLS1_SendHelpStr("  sniff on|off", "Turns packet sniffing on or off\r\n", io->stdOut);
  CLS1_SendHelpStr("  channel <number>", "Switches to the given channel. Channel must be in the range 0..15\r\n", io->stdOut);
  CLS1_SendHelpStr("  power <number>", "Changes the output power. Power must be in the range 0..15\r\n", io->stdOut);
  CLS1_SendHelpStr("  send <string>", "Send a string using the wireless transceiver\r\n", io->stdOut);
#if PL_HAS_CONTEST
  CLS1_SendHelpStr("  challenge a op b = ?", "Send a contest using the wireless transceiver\r\n", io->stdOut);
  CLS1_SendHelpStr("  contest on|off|<nr>", "Turns the radio contest on or off or specifies contest number\r\n", io->stdOut);
#endif
}

static void RADIO_PrintStatus(const CLS1_StdIOType *io) {
  short dBm;
  unsigned char link_quality;  /* Holds the link quality of the last received Packet.*/

  CLS1_SendStatusStr("Radio", "\r\n", io->stdOut);
  CLS1_SendStatusStr("  transceiver", RADIO_isOn?"on\r\n":"off\r\n", io->stdOut);
  CLS1_SendStatusStr("  sniffing", RADIO_isSniffing?"on\r\n":"off\r\n", io->stdOut);
  link_quality = SMAC1_MLMELinkQuality();  /* Read the link quality of the last received packet.*/
  dBm = (short)(-(link_quality/2));
  CLS1_SendStatusStr("  LQ", "", io->stdOut); CLS1_SendNum16s(dBm, io->stdOut); CLS1_SendStr(" dBm\r\n", io->stdOut);
  CLS1_SendStatusStr("  channel", "", io->stdOut); CLS1_SendNum16u((uint16_t)RADIO_Channel, io->stdOut); CLS1_SendStr("\r\n", io->stdOut);
  CLS1_SendStatusStr("  outputPower", "", io->stdOut); CLS1_SendNum16u((uint16_t)RADIO_OutputPower, io->stdOut); CLS1_SendStr("\r\n", io->stdOut);
  CLS1_SendStatusStr("  PAIND", RADIO_PREFIX_STR, io->stdOut); CLS1_SendStr("\r\n", io->stdOut);
  CLS1_SendStatusStr("  ACK", RADIO_ACK_STR, io->stdOut); CLS1_SendStr("\r\n", io->stdOut);
#if PL_HAS_CONTEST
  CLS1_SendStatusStr("  contest", RADIO_isContest?"yes, #:":"no, #:", io->stdOut);
  CLS1_SendNum8u(RADIO_contestNo, io->stdOut);
  CLS1_SendStr("\r\n", io->stdOut);
#endif
}

uint8_t RADIO_ParseCommand(const char *cmd, bool *handled, const CLS1_StdIOType *io) {
  uint8_t res = ERR_OK;
  long val;
  const char *p;

  if (UTIL1_strcmp(cmd, CLS1_CMD_HELP)==0 || UTIL1_strcmp(cmd, "radio help")==0) {
    RADIO_PrintHelp(io);
    *handled = TRUE;
  } else if (UTIL1_strcmp(cmd, CLS1_CMD_STATUS)==0 || UTIL1_strcmp(cmd, "radio status")==0) {
    RADIO_PrintStatus(io);
    *handled = TRUE;
  } else if (UTIL1_strcmp(cmd, "radio on")==0) {
    RADIO_isOn = TRUE;
    *handled = TRUE;
  } else if (UTIL1_strcmp(cmd, "radio off")==0) {
    RADIO_isOn = FALSE;
    *handled = TRUE;
  } else if (UTIL1_strcmp(cmd, "radio sniff on")==0) {
    RADIO_isSniffing = TRUE;
    *handled = TRUE;
  } else if (UTIL1_strcmp(cmd, "radio sniff off")==0) {
    RADIO_isSniffing = FALSE;
    *handled = TRUE;
#if PL_HAS_CONTEST
  } else if (UTIL1_strcmp(cmd, "radio contest on")==0) {
    RADIO_isContest = TRUE;
    *handled = TRUE;
  } else if (UTIL1_strcmp(cmd, "radio contest off")==0) {
    RADIO_isContest = FALSE;
    *handled = TRUE;
  } else if (UTIL1_strncmp(cmd, "radio contest ", sizeof("radio contest ")-1)==0) {
    p = cmd+sizeof("radio contest ")-1;
    RADIO_contestNo = (uint8_t)(*p-'0'); /* single digit only */
    *handled = TRUE;
  } else if (UTIL1_strncmp(cmd, "radio challenge", sizeof("radio challenge")-1)==0) {
    bool isQuestion;
    int8_t a, b;
    unsigned char op;

    p = cmd+sizeof("radio challenge");
    if (IsContestMessage((unsigned char*)p, &isQuestion, &a, &b, &op)) {
      unsigned char buf[16];

      UTIL1_Num8sToStr(buf, sizeof(buf), a);
      UTIL1_chcat(buf, sizeof(buf), ' ');
      UTIL1_chcat(buf, sizeof(buf), op);
      UTIL1_chcat(buf, sizeof(buf), ' ');
      UTIL1_strcatNum8s(buf, sizeof(buf), b);
      UTIL1_strcat(buf, sizeof(buf), " = ?");
      RADIO_SendStringRaw(buf);
      CalcResultString(ContestCalcExpectedResultStr, sizeof(ContestCalcExpectedResultStr), a, b, op);
    }
    *handled = TRUE;
#endif
  } else if (UTIL1_strncmp(cmd, "radio channel", sizeof("radio channel")-1)==0) {
    p = cmd+sizeof("radio channel");
    if (UTIL1_xatoi(&p, &val)==ERR_OK && val>=0 && val<=15) {
      RADIO_SetChannel((uint8_t)val);
      *handled = TRUE;
    } else {
      CLS1_SendStr("Wrong argument, must be in the range 0..15\r\n", io->stdErr);
      res = ERR_FAILED;
    }
  } else if (UTIL1_strncmp(cmd, "radio power", sizeof("radio power")-1)==0) {
    p = cmd+sizeof("radio power");
    if (UTIL1_xatoi(&p, &val)==ERR_OK && val>=0 && val<=15) {
      RADIO_SetOutputPower((uint8_t)val);
      *handled = TRUE;
    } else {
      CLS1_SendStr("Wrong argument, must be in the range 0..15\r\n", io->stdErr);
      res = ERR_FAILED;
    }
  } else if (UTIL1_strncmp(cmd, "radio send", sizeof("radio send")-1)==0) {
    p = cmd+sizeof("radio send");
    RADIO_SendString(p);
    *handled = TRUE;
  }
  return res;
}

/*!
  \brief This routine is called as callback by the radio driver on reception of a data packet
  \param msg Pointer to the message we received.
 */
static void RADIO_HandleMessage(uint8_t *msg) {
  char buf[32];
  uint8_t i, size;

  if (RADIO_isSniffing && *msg==RADIO_QUEUE_MSG_SNIFF) {
    msg++;
    size = *msg++;
    UTIL1_strcpy(buf, sizeof(buf), "\r\nch #:"); /* use new line at the beginning, as the hex dump at the end might be fill up buffer completely */
    UTIL1_strcatNum16s(buf, sizeof(buf), RADIO_Channel);
    UTIL1_strcat(buf, sizeof(buf), " size:");
    UTIL1_strcatNum16s(buf, sizeof(buf), size);
    UTIL1_strcat(buf, sizeof(buf), " ASCII: ");
    SHELL_SendMessage(buf);
    buf[0] = '\0';
    /* write as string */
    for(i=0;i<size && i<sizeof(buf);i++) {
      UTIL1_chcat(buf, sizeof(buf), msg[i]);
    }
    SHELL_SendMessage(buf);
    /* write as hex */
    buf[0] = '\0';
    UTIL1_strcat(buf, sizeof(buf), " hex: ");
    for(i=0; i<size;i++) {
      UTIL1_strcatNum8Hex(buf, sizeof(buf), msg[i]);
      UTIL1_strcat(buf, sizeof(buf), " ");
    }
    SHELL_SendMessage(buf);
    SHELL_SendMessage("\r\n");
#if PL_HAS_CONTEST
  } else if (*msg==RADIO_QUEUE_MSG_CONTEST_QUESTION) {
    msg++;
    size = *msg++;
    CLS1_SendStr("\r\nContest question: ", CLS1_GetStdio()->stdOut);
    CLS1_SendStr(msg, CLS1_GetStdio()->stdOut);
    CLS1_SendStr("\r\n", CLS1_GetStdio()->stdOut);
    RespondWithAnswer(msg);
  } else if (*msg==RADIO_QUEUE_MSG_CONTEST_ANSWER) {
    msg++;
    size = *msg++;
    if (UTIL1_strncmp(msg, ContestCalcExpectedResultStr, UTIL1_strlen(ContestCalcExpectedResultStr))) {
      CLS1_SendStr("\r\n****FOUND A WINNER!!!!!!\r\n", CLS1_GetStdio()->stdOut);
      CLS1_SendStr(msg, CLS1_GetStdio()->stdOut);
      CLS1_SendStr("\r\n", CLS1_GetStdio()->stdOut);
    }
#endif
#if PL_HAS_REMOTE && PL_HAS_MOTOR
  /*! \todo Implement handling for your remote control */
  } else if (*msg==RADIO_QUEUE_MSG_ACCEL) {
    msg++;
    size = *msg++;
    REMOTE_ParseMsg((const char*)msg+sizeof(RADIO_PREFIX_STR)-1, size-sizeof(RADIO_PREFIX_STR)-1);
#endif
  }
}

/*! \brief Radio application state machine */
void RADIO_Handle(void) {
  uint8_t buf[RADIO_QUEUE_ITEM_SIZE];

  if (RADIO_isOn) {
    RADIO_HandleState(); /* advance state machine */
  }
  /* poll radio message queue */
  if (FRTOS1_xQueueReceive(RADIO_MsgQueue, buf, 0)==pdPASS) {
    /* received message from queue */
    RADIO_HandleMessage(buf);
  }
}

#if PL_HAS_CONTEST
typedef struct {
  int8_t a, b;
  unsigned char op;
} ContestOperation;

static ContestOperation contest0[] = {
  {2, 7, '+'},
  {3, 10, '+'},
  {17, 5, '+'},
};

static ContestOperation contest1[] = {
  {2, 7, '+'},
  {3, 10, '+'},
  {17, 5, '+'},
  {3, 2, '/'},
  {5, 6, '*'},
  {10, 3, '-'},
};

#if 0
static void SendContest(ContestOperation *contest, uint8_t nofOps) {
  uint8_t i;
  unsigned char buf[16];
  uint8_t channel = 0;

  for(i=0;i<nofOps;i++) {
    #if 0
    if (channel < 15) { /* valid from 0..15 */
      channel++;
    } else {
      channel = 0;
    }
    RADIO_SetChannel(channel);
    #endif
    UTIL1_Num8sToStr(buf, sizeof(buf), contest[i].a);
    UTIL1_chcat(buf, sizeof(buf), ' ');
    UTIL1_chcat(buf, sizeof(buf), contest[i].op);
    UTIL1_chcat(buf, sizeof(buf), ' ');
    UTIL1_strcatNum8s(buf, sizeof(buf), contest[i].b);
    UTIL1_strcat(buf, sizeof(buf), " = ?");
    RADIO_SendStringRaw(buf);
    CalcResultString(ContestCalcExpectedResultStr, sizeof(ContestCalcExpectedResultStr), contest[i].a, contest[i].b, contest[i].op);
    FRTOS1_vTaskDelay(250/portTICK_RATE_MS);
  } /* for */
}
#endif

#if 0
static portTASK_FUNCTION(ContestTask, pvParameters) {
  (void)pvParameters; /* parameter not used */
  for(;;) {
    if (RADIO_isContest) {
      if (RADIO_contestNo==0) {
        SendContest(contest0, sizeof(contest0)/sizeof(ContestOperation));
      } else if (RADIO_contestNo==1) {
        SendContest(contest1, sizeof(contest1)/sizeof(ContestOperation));
      }
    } /* contest */
    FRTOS1_vTaskDelay(250/portTICK_RATE_MS);
  } /* for */
}
#endif
#endif

void RADIO_Init(void) {
  (void)SMAC1_MLMESetMC13192ClockRate(0);    /* Set initial Clk speed from transceiver (CLKO)*/
  RADIO_SetChannel(RADIO_Channel);           /* Set channel */
  RADIO_SetOutputPower(15);                  /* Set output power */

  /*Initialize the packet */
  RADIO_TxPacket.u8DataLength = 0;               /* Set TX default length to 0 */
  RADIO_TxPacket.pu8Data = &RADIO_TxDataBuffer[0]; /* Load the address of our txbuffer into the tx structure */

  RADIO_RxPacket.u8DataLength = 0;               /* Set RX default to 0*/
  RADIO_RxPacket.pu8Data = &RADIO_RxDataBuffer[0];  /* Load the address of our rxbuffer into rx structure */
  RADIO_RxPacket.u8MaxDataLength = 24;           /* Define the max buffer we are interested in */
  RADIO_RxPacket.u8Status = TRSVR1_INITIAL_VALUE;  /* initialize the status packet to 0 */

  RADIO_AppStatus = RADIO_INITIAL_STATE;        /* Set the initial status of the application state variable */
  /* create data queue */
  RADIO_MsgQueue = FRTOS1_xQueueCreate(RADIO_QUEUE_NOF_ITEMS, RADIO_QUEUE_ITEM_SIZE);
  if (RADIO_MsgQueue==NULL) { /* queue creation failed! */
    for(;;) {} /* not enough memory? */
  }
#if PL_HAS_RTOS_TRACE
  Ptrc1_vTraceSetQueueName(RADIO_MsgQueue, "RadioQueue");
#endif
#if PL_HAS_RTOS_ISR_TRACE
  /* need to set this up *before* it fires the interrupts */
  Ptrc1_vTraceSetISRProperties(ID_ISR_RADIO, "ISRRadio", PRIO_OF_ISR_RADIO);
#endif
#if 0 && PL_HAS_CONTEST && PL_IS_SRB_BOARD /* only for sender */
  if (FRTOS1_xTaskCreate(
      ContestTask,  /* pointer to the task */
        (signed portCHAR *)"Contest", /* task name for kernel awareness debugging */
        configMINIMAL_STACK_SIZE+200, /* task stack size */
        (void*)NULL, /* optional task startup argument */
        tskIDLE_PRIORITY,  /* initial priority */
        (xTaskHandle*)NULL /* optional task handle to create */
      ) != pdPASS) {
    /*lint -e527 */
    for(;;){}; /* error! probably out of memory */
    /*lint +e527 */
  }
#endif
}
#endif
