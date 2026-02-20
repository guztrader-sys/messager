#pragma once


typedef struct waiting_process *WaitingProcessPtr;
typedef struct mail_slot *SlotPtr;
typedef struct mailbox MailBox;

typedef enum {MB_ZEROSLOT=0, MB_SINGLESLOT, MB_MULTISLOT, MB_MAXTYPES} MAILBOX_TYPE;
typedef enum {MBSTATUS_EMPTY=0, MBSTATUS_INUSE, MBSTATUS_RELEASED, MBSTATUS_MAX} MAILBOX_STATUS;

/* Block status values for use with block() */
#define BLOCKED_RECEIVE 11
#define BLOCKED_SEND    12
#define BLOCKED_RELEASE 13

typedef struct mail_slot 
{
   SlotPtr   pNextSlot;
   SlotPtr   pPrevSlot;
   int       mbox_id;
   unsigned char message[MAX_MESSAGE];
   int       messageSize;
   /* other items as needed... */

} MailSlot;

typedef struct waiting_process
{
   WaitingProcessPtr    pNextProcess;
   WaitingProcessPtr    pPrevProcess;
   int                  pid;
   /* other items as needed... */
} WaitingProcess;

struct mailbox 
{
   SlotPtr      pSlotListHead;
   int           mbox_id;
   int           blockedSenderCount; /* Number of blocked senders */
   int           blockedSenderQueue[MAXSLOTS]; /* FIFO queue of blocked sender PIDs */
   int           blockedSenderHead;
   int           blockedSenderTail;
   int           releaseBlockedSenders; /* Set to 1 by mailbox_free to signal all blocked senders */
   int           blockedReceiverCount; /* Number of blocked receivers (for zero-slot mailboxes) */
   int           blockedReceiverQueue[MAXSLOTS]; /* FIFO queue of blocked receiver PIDs */
   int           blockedReceiverHead;
   int           blockedReceiverTail;
   int           releaseBlockedReceivers; /* Set to 1 by mailbox_free to signal all blocked receivers */
   /* other items as needed... */
   MAILBOX_TYPE      type;
   MAILBOX_STATUS    status;
   int               slotSize;
   int               slotCount;
};



