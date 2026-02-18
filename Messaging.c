/* ------------------------------------------------------------------------
   Messaging.c
   College of Applied Science and Technology
   The University of Arizona
   CYBV 489

   Student Names:  <add your group members here>

   ------------------------------------------------------------------------ */
#include <Windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <THREADSLib.h>
#include <Scheduler.h>
#include <Messaging.h>
#include <stdint.h>
#include "message.h"

/* ------------------------- Prototypes ----------------------------------- */
static void nullsys(system_call_arguments_t* args);

/* Note: interrupt_handler_t is already defined in THREADSLib.h with the signature:
 *   void (*)(char deviceId[32], uint8_t command, uint32_t status, void *pArgs)
 */

static void InitializeHandlers();
static int check_io_messaging(void);
extern int MessagingEntryPoint(void*);
static void checkKernelMode(const char* functionName);

struct psr_bits {
    unsigned int cur_int_enable : 1;
    unsigned int cur_mode : 1;
    unsigned int prev_int_enable : 1;
    unsigned int prev_mode : 1;
    unsigned int unused : 28;
};

union psr_values {
    struct psr_bits bits;
    unsigned int integer_part;
};


/* -------------------------- Globals ------------------------------------- */

/* Obtained from THREADS*/
interrupt_handler_t* handlers;

/* system call array of function pointers */
void (*systemCallVector[THREADS_MAX_SYSCALLS])(system_call_arguments_t* args);

/* the mail boxes */
MailBox mailboxes[MAXMBOX];
MailSlot mailSlots[MAXSLOTS];

typedef struct
{
    void* deviceHandle;
    int deviceMbox;
    int deviceType;
    char deviceName[16];
} DeviceManagementData;

static DeviceManagementData devices[THREADS_MAX_DEVICES];
static int nextMailboxId = 0;
static int waitingOnDevice = 0;


/* ------------------------------------------------------------------------
     Name - SchedulerEntryPoint
     Purpose - Initializes mailboxes and interrupt vector.
               Start the Messaging test process.
     Parameters - one, default arg passed by k_spawn that is not used here.
----------------------------------------------------------------------- */
int SchedulerEntryPoint(void* arg)
{
    // TODO: check for kernel mode
    checkKernelMode("SchedulerEntryPoint");
    uint32_t psr = get_psr();
    int kernelMode = psr & PSR_KERNEL_MODE != 0;
    if (!kernelMode)
    {        console_output(FALSE, "SchedulerEntryPoint called in kernel mode. Halting...\n");
        stop(1);
    }
    else
    {
        console_output(FALSE, "SchedulerEntryPoint called in kernel mode. Continuing...\n");
    }
    /* Disable interrupts */
    disableInterrupts();

    /* set this to the real check_io function. */
    check_io = check_io_messaging;

    /* Initialize the mail box table, slots, & other data structures.
     * Initialize int_vec and sys_vec, allocate mailboxes for interrupt
     * handlers.  Etc... */

    /* Initialize the devices and their mailboxes. */
    
    /* Allocate mailboxes for use by the interrupt handlers.
     * Note: The clock device uses a zero-slot mailbox, while I/O devices
     * (disks, terminals) need slotted mailboxes since their interrupt
     * handlers use non-blocking sends.
     */
    // TODO: Create mailboxes for each device.
    //   devices[THREADS_CLOCK_DEVICE_ID].deviceMbox = mailbox_create(0, sizeof(int));
    //   devices[i].deviceMbox = mailbox_create(..., sizeof(int));

    /* TODO: Initialize the devices using device_initialize().
     * The devices are: disk0, disk1, term0, term1, term2, term3.
     * Store the device handle and name in the devices array.
     */

    InitializeHandlers();

    enableInterrupts();

    /* Create a process for Messaging, then block on a wait until messaging exits.*/
    int messagingPid = k_spawn("MessagingEntryPoint", MessagingEntryPoint, NULL, THREADS_MIN_STACK_SIZE, LOWEST_PRIORITY);
    if (messagingPid < 0)
    {
        console_output(FALSE, "Failed to spawn MessagingEntryPoint process.\n");
        stop(1);
    }

    /* Wait for the messaging process to exit */
    int exitCode = 0;
    k_wait(&exitCode);

    k_exit(0);

    return 0;
} /* SchedulerEntryPoint */


/* ------------------------------------------------------------------------
   Name - mailbox_create
   Purpose - gets a free mailbox from the table of mailboxes and initializes it
   Parameters - maximum number of slots in the mailbox and the max size of a msg
                sent to the mailbox.
   Returns - -1 to indicate that no mailbox was created, or a value >= 0 as the
             mailbox id.
   ----------------------------------------------------------------------- */
int mailbox_create(int slots, int slot_size)
{
    int newId = -1;

    /* Check if we've exceeded the maximum number of mailboxes */
    if (nextMailboxId >= MAXMBOX)
    {
        return -1;
    }

    /* Validate slot_size against MAX_MESSAGE */
    if (slot_size > MAX_MESSAGE)
    {
        return -1;
    }

    /* Get the next available mailbox */
    newId = nextMailboxId;
    nextMailboxId++;

    /* Initialize the mailbox */
    mailboxes[newId].mbox_id = newId;
    mailboxes[newId].slotCount = slots;
    mailboxes[newId].slotSize = slot_size;
    mailboxes[newId].pSlotListHead = NULL;
    mailboxes[newId].status = MBSTATUS_INUSE;

    /* Determine mailbox type based on slot count */
    if (slots == 0)
    {
        mailboxes[newId].type = MB_ZEROSLOT;
    }
    else if (slots == 1)
    {
        mailboxes[newId].type = MB_SINGLESLOT;
    }
    else
    {
        mailboxes[newId].type = MB_MULTISLOT;
    }

    return newId;
} /* mailbox_create */


/* ------------------------------------------------------------------------
   Name - mailbox_send
   Purpose - Put a message into a slot for the indicated mailbox.
             Block the sending process if no slot available.
   Parameters - mailbox id, pointer to data of msg, # of bytes in msg,
                block flag.
   Returns - zero if successful, -1 if invalid args, -2 if would block
             (non-blocking mode), -5 if signaled while waiting.
   Side Effects - none.
   ----------------------------------------------------------------------- */
int mailbox_send(int mboxId, void* pMsg, int msg_size, int wait)
{
    int result = 0;

    /* Validate mailbox id */
    if (mboxId < 0 || mboxId >= MAXMBOX)
    {
        return -1;
    }

    /* Validate mailbox status */
    if (mailboxes[mboxId].status != MBSTATUS_INUSE)
    {
        return -1;
    }

    /* Validate message size */
    if (msg_size > mailboxes[mboxId].slotSize)
    {
        return -1;
    }

    /* Check if mailbox is full (all slots used) */
    MailSlot* pSlot = mailboxes[mboxId].pSlotListHead;
    int slotCount = 0;
    while (pSlot != NULL)
    {
        slotCount++;
        pSlot = pSlot->pNextSlot;
    }

    /* If mailbox is full and we can't block, return error */
    if (slotCount >= mailboxes[mboxId].slotCount && !wait)
    {
        return -2;
    }

    /* If mailbox is full and we should block, block the process */
    if (slotCount >= mailboxes[mboxId].slotCount && wait)
    {
        /* Block on send */
        block(BLOCKED_SEND);

        if (signaled())
        {
            return -5;
        }
    }

    /* Check if we have available slots */
    if (slotCount < MAXSLOTS)
    {
        /* Allocate a new slot */
        pSlot = &mailSlots[slotCount];
        pSlot->mbox_id = mboxId;
        pSlot->messageSize = msg_size;
        memcpy(pSlot->message, pMsg, msg_size);
        pSlot->pNextSlot = NULL;
        pSlot->pPrevSlot = NULL;

        /* Add to mailbox's slot list */
        if (mailboxes[mboxId].pSlotListHead == NULL)
        {
            mailboxes[mboxId].pSlotListHead = pSlot;
        }
        else
        {
            MailSlot* pTemp = mailboxes[mboxId].pSlotListHead;
            while (pTemp->pNextSlot != NULL)
            {
                pTemp = pTemp->pNextSlot;
            }
            pTemp->pNextSlot = pSlot;
            pSlot->pPrevSlot = pTemp;
        }

        result = 0;
    }
    else
    {
        result = -1;
    }

    return result;
}

/* ------------------------------------------------------------------------
   Name - mailbox_receive
   Purpose - Receive a message from the indicated mailbox.
             Block the receiving process if no message available.
   Parameters - mailbox id, pointer to buffer for msg, max size of buffer,
                block flag.
   Returns - size of received msg (>=0) if successful, -1 if invalid args,
             -2 if would block (non-blocking mode), -5 if signaled.
   Side Effects - none.
   ----------------------------------------------------------------------- */
int mailbox_receive(int mboxId, void* pMsg, int msg_size, int wait)
{
    int result = -1;

    /* Validate mailbox id */
    if (mboxId < 0 || mboxId >= MAXMBOX)
    {
        return -1;
    }

    /* Validate mailbox status */
    if (mailboxes[mboxId].status != MBSTATUS_INUSE)
    {
        return -1;
    }

    /* Check if mailbox has any messages */
    MailSlot* pSlot = mailboxes[mboxId].pSlotListHead;

    /* If no messages available and not blocking, return error */
    if (pSlot == NULL && !wait)
    {
        return -2;
    }

    /* If no messages available and should block, block the process */
    if (pSlot == NULL && wait)
    {
        /* Block on receive */
        block(BLOCKED_RECEIVE);

        if (signaled())
        {
            return -5;
        }

        /* After unblocking, check for messages again */
        pSlot = mailboxes[mboxId].pSlotListHead;
    }

    /* Receive the message from the mailbox */
    if (pSlot != NULL)
    {
        /* Check if buffer is large enough */
        if (pSlot->messageSize > msg_size)
        {
            return -1;
        }

        /* Copy message to buffer */
        memcpy(pMsg, pSlot->message, pSlot->messageSize);
        result = pSlot->messageSize;

        /* Remove slot from mailbox's slot list */
        if (pSlot->pNextSlot != NULL)
        {
            pSlot->pNextSlot->pPrevSlot = pSlot->pPrevSlot;
        }
        if (pSlot->pPrevSlot != NULL)
        {
            pSlot->pPrevSlot->pNextSlot = pSlot->pNextSlot;
        }
        else
        {
            mailboxes[mboxId].pSlotListHead = pSlot->pNextSlot;
        }

        /* Mark slot as available */
        pSlot->pNextSlot = NULL;
        pSlot->pPrevSlot = NULL;
    }

    return result;
}

/* ------------------------------------------------------------------------
   Name - mailbox_free
   Purpose - Frees a previously created mailbox. Any process waiting on
             the mailbox should be signaled and unblocked.
   Parameters - mailbox id.
   Returns - zero if successful, -1 if invalid args, -5 if signaled
             while closing the mailbox.
   ----------------------------------------------------------------------- */
int mailbox_free(int mboxId)
{
    int result = 0;

    /* Validate mailbox id */
    if (mboxId < 0 || mboxId >= MAXMBOX)
    {
        return -1;
    }

    /* Validate mailbox status */
    if (mailboxes[mboxId].status != MBSTATUS_INUSE)
    {
        return -1;
    }

    /* Mark mailbox as released */
    mailboxes[mboxId].status = MBSTATUS_RELEASED;

    /* Clear all slots associated with this mailbox */
    MailSlot* pSlot = mailboxes[mboxId].pSlotListHead;
    while (pSlot != NULL)
    {
        MailSlot* pNext = pSlot->pNextSlot;
        pSlot->pNextSlot = NULL;
        pSlot->pPrevSlot = NULL;
        pSlot = pNext;
    }
    mailboxes[mboxId].pSlotListHead = NULL;

    /* Check if signaled while closing */
    if (signaled())
    {
        result = -5;
    }

    return result;
}

/* ------------------------------------------------------------------------
   Name - wait_device
   Purpose - Waits for a device interrupt by blocking on the device's
             mailbox. Returns the device status via the status pointer.
   Parameters - device name string, pointer to status output.
   Returns - 0 if successful, -1 if invalid parameter, -5 if signaled.
   ----------------------------------------------------------------------- */
int wait_device(char* deviceName, int* status)
{
    int result = 0;
    uint32_t deviceHandle = -1;
    checkKernelMode("waitdevice");

    enableInterrupts();

    if (strcmp(deviceName, "clock") == 0)
    {
        deviceHandle = THREADS_CLOCK_DEVICE_ID;
    }
    else
    {
        deviceHandle = device_handle(deviceName);

    }

    if (deviceHandle >= 0 && deviceHandle < THREADS_MAX_DEVICES)
    {
        /* set a flag that there is a process waiting on a device. */
        waitingOnDevice++;
        mailbox_receive(devices[deviceHandle].deviceMbox, status, sizeof(int), TRUE);

        disableInterrupts();

        waitingOnDevice--;
    }
    else
    {
        console_output(FALSE, "Unknown device type.");
        stop(-1);
    }

    /* spec says return -5 if signaled. */
    if (signaled())
    {
        result = -5;
    }

    return result;
}


int check_io_messaging(void)
{
    if (waitingOnDevice)
    {
        return 1;
    }
    return 0;
}

static void InitializeHandlers()
{
    handlers = get_interrupt_handlers();

    /* Register interrupt handlers in the handlers array.
     * Use the interrupt indices defined in THREADSLib.h:
     *   handlers[THREADS_TIMER_INTERRUPT]   = your_clock_handler;
     *   handlers[THREADS_IO_INTERRUPT]      = your_io_handler;
     *   handlers[THREADS_SYS_CALL_INTERRUPT] = your_syscall_handler;
     *
     * Also initialize the system call vector (systemCallVector).
     */

    /* For MessagingTest00, we need to initialize the system call vector
     * with null handlers initially. This is a minimal implementation.
     */
    int i;
    for (i = 0; i < THREADS_MAX_SYSCALLS; i++)
    {
        systemCallVector[i] = nullsys;
    }
}

/* an error method to handle invalid syscalls */
static void nullsys(system_call_arguments_t* args)
{
    console_output(FALSE,"nullsys(): Invalid syscall %d. Halting...\n", args->call_id);
    stop(1);
} /* nullsys */


/*****************************************************************************
   Name - checkKernelMode
   Purpose - Checks the PSR for kernel mode and halts if in user mode
   Parameters -
   Returns -
****************************************************************************/
static inline void checkKernelMode(const char* functionName)
{
    union psr_values psrValue;

    psrValue.integer_part = get_psr();
    if (psrValue.bits.cur_mode == 0)
    {
        console_output(FALSE, "Kernel mode expected, but function called in user mode.\n");
        stop(1);
    }
}
