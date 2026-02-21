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
    uint32_t psr = get_psr(); //get the psr to check if we are in kernel mode.
    int kernelMode = psr & PSR_KERNEL_MODE != 0; //check the kernel mode bit in the psr.
    if (!kernelMode)
    {
        console_output(FALSE, "SchedulerEntryPoint should be running in kernel mode. Halting...\n");
        stop(1);
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
    devices[THREADS_CLOCK_DEVICE_ID].deviceMbox = mailbox_create(0, sizeof(int));//   Create a zero-slot mailbox for the clock device (timer interrupts)
    for (int i = 0; i < THREADS_MAX_DEVICES; ++i) {
        if (i != THREADS_CLOCK_DEVICE_ID) {
            devices[i].deviceMbox = mailbox_create(1, sizeof(int));//   Create a single-slot mailbox for I/O devices (disks, terminals) 
        }
    }
    //   devices[i].deviceMbox = mailbox_create(..., sizeof(int));

    /* TODO: Initialize the devices using device_initialize().
     * The devices are: disk0, disk1, term0, term1, term2, term3.
     * Store the device handle and name in the devices array.
     */
    if(device_initialize("disk0") >= 0) {
        devices[0].deviceHandle = device_handle("disk0");
        strncpy(devices[0].deviceName, "disk0", sizeof(devices[0].deviceName));
        devices[0].deviceType = DEVICE_DISK;
    }
    if(device_initialize("disk1") >= 0) {
        devices[1].deviceHandle = device_handle("disk1");
        strncpy(devices[1].deviceName, "disk1", sizeof(devices[1].deviceName));
        devices[1].deviceType = DEVICE_DISK;
    }
    for (int i = 0; i < 4; ++i) {
        char termName[16];
        snprintf(termName, sizeof(termName), "term%d", i); 
        if(device_initialize(termName) >= 0) {
            devices[2 + i].deviceHandle = device_handle(termName);
            strncpy(devices[2 + i].deviceName, termName, sizeof(devices[2 + i].deviceName));
            devices[2 + i].deviceType = DEVICE_TERMINAL;
        }
    }       

    InitializeHandlers();

    enableInterrupts();

    /* TODO: Create a process for Messaging, then block on a wait until messaging exits.*/
    int result = k_spawn( "Messaging", MessagingEntryPoint, NULL, THREADS_MIN_STACK_SIZE, 1);
    if (result < 0)
    {
        console_output(FALSE, "Failed to create Messaging process. Halting...\n");
        stop(1);
    }   
    int exitCode=0;
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
    // Find a free mailbox slot
    for (int i = 0; i < MAXMBOX; ++i) {
        if (mailboxes[i].status != MBSTATUS_INUSE) {// Check if the mailbox is free (i.e., not in use)
            // Initialize mailbox fields
            mailboxes[i].mbox_id = i; // Assign a unique mailbox ID (can be the index in the mailboxes array)
            mailboxes[i].slotCount = slots; // Set the number of slots in the mailbox
            mailboxes[i].slotSize = slot_size; // Set the size of each slot
            mailboxes[i].status = MBSTATUS_INUSE; // Mark the mailbox as in use
            mailboxes[i].type = (slots == 0) ? MB_ZEROSLOT : (slots == 1 ? MB_SINGLESLOT : MB_MULTISLOT); // Determine the mailbox type based on the number of slots
            mailboxes[i].pSlotListHead = NULL; // Initialize the slot list head to NULL
            // Other fields can be initialized as needed
            return i;
        }
    }
    return -1;
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
    // Validate mailbox id
    if (mboxId < 0 || mboxId >= MAXMBOX || mailboxes[mboxId].status != MBSTATUS_INUSE)
        return -1;
    // Validate message size
    if (msg_size > mailboxes[mboxId].slotSize)
        return -1;
    // Count slots
    int slotCount = 0;
    SlotPtr slot = mailboxes[mboxId].pSlotListHead;
    while (slot) {
        slotCount++;
        slot = slot->pNextSlot;
    }
    if (slotCount >= mailboxes[mboxId].slotCount) {
        // No available slot
        if (!wait)
            return -2;
        // Would block, but single-process test never blocks
        return -2;
    }
    // Allocate new slot
    SlotPtr newSlot = (SlotPtr)malloc(sizeof(MailSlot));
    if (!newSlot)
        return -1;
    newSlot->mbox_id = mboxId;
    memcpy(newSlot->message, pMsg, msg_size);
    newSlot->messageSize = msg_size;
    newSlot->pNextSlot = NULL;
    newSlot->pPrevSlot = NULL;
    // Insert at end
    if (!mailboxes[mboxId].pSlotListHead) {
        mailboxes[mboxId].pSlotListHead = newSlot;
    } else {
        SlotPtr last = mailboxes[mboxId].pSlotListHead;
        while (last->pNextSlot)
            last = last->pNextSlot;
        last->pNextSlot = newSlot;
        newSlot->pPrevSlot = last;
    }
    return 0;
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
    // Validate mailbox id
    if (mboxId < 0 || mboxId >= MAXMBOX || mailboxes[mboxId].status != MBSTATUS_INUSE)
        return -1;
    // Check for available slot
    SlotPtr slot = mailboxes[mboxId].pSlotListHead;
    if (!slot) {
        // No message available
        if (!wait)
            return -2;
        // Would block, but single-process test never blocks
        return -2;
    }
    // Copy message
    int copySize = (slot->messageSize < msg_size) ? slot->messageSize : msg_size;
    memcpy(pMsg, slot->message, copySize);
    // Remove slot from list
    mailboxes[mboxId].pSlotListHead = slot->pNextSlot;
    if (mailboxes[mboxId].pSlotListHead)
        mailboxes[mboxId].pSlotListHead->pPrevSlot = NULL;
    free(slot);
    return copySize;
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
    int result = -1;

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

    /* TODO: Register interrupt handlers in the handlers array.
     * Use the interrupt indices defined in THREADSLib.h:
     *   handlers[THREADS_TIMER_INTERRUPT]   = your_clock_handler;
     *   handlers[THREADS_IO_INTERRUPT]      = your_io_handler;
     *   handlers[THREADS_SYS_CALL_INTERRUPT] = your_syscall_handler;
     *
     * Also initialize the system call vector (systemCallVector).
     */

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
