/*
 * downstream_msc.c
 *
 *  Created on: 8/08/2015
 *      Author: Robert Fisk
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */


#include "downstream_msc.h"
#include "downstream_interface_def.h"
#include "downstream_statemachine.h"
#include "downstream_spi.h"
#include "usbh_msc.h"
#include "build_config.h"


#ifdef CONFIG_MASS_STORAGE_ENABLED

extern USBH_HandleTypeDef hUsbHostFS;       //Hard-link ourselves to usb_host.c


//Stuff we need to save for our callbacks to use:
DownstreamMSCCallbackPacketTypeDef  GetStreamDataCallback;
uint32_t                            ByteCount;
DownstreamPacketTypeDef*            ReadStreamPacket;
uint8_t                             ReadStreamBusy;


static void Downstream_MSC_PacketProcessor_TestUnitReady(DownstreamPacketTypeDef* receivedPacket);
static void Downstream_MSC_PacketProcessor_TestUnitReadyCallback(USBH_StatusTypeDef result);
static void Downstream_MSC_PacketProcessor_GetCapacity(DownstreamPacketTypeDef* receivedPacket);
static void Downstream_MSC_PacketProcessor_BeginRead(DownstreamPacketTypeDef* receivedPacket);
static void Downstream_MSC_PacketProcessor_BeginWrite(DownstreamPacketTypeDef* receivedPacket);
static void Downstream_MSC_PacketProcessor_RdWrCompleteCallback(USBH_StatusTypeDef result);
static void Downstream_MSC_GetStreamDataPacketCallback(DownstreamPacketTypeDef* receivedPacket);
static void Downstream_MSC_PacketProcessor_Disconnect(DownstreamPacketTypeDef* receivedPacket);
static void Downstream_MSC_PacketProcessor_DisconnectCallback(USBH_StatusTypeDef result);


//High-level checks on the connected device. We don't want some weirdly
//configured device to bomb our USB stack, accidentally or otherwise.
InterfaceCommandClassTypeDef Downstream_MSC_ApproveConnectedDevice(void)
{
    MSC_HandleTypeDef* MSC_Handle =  (MSC_HandleTypeDef*)hUsbHostFS.pActiveClass->pData;

    if (MSC_Handle->unit[MSC_FIXED_LUN].error != MSC_OK)
    {
        return COMMAND_CLASS_INTERFACE;         //fail
    }

    if ((MSC_Handle->unit[MSC_FIXED_LUN].capacity.block_nbr == 0) ||
        (MSC_Handle->unit[MSC_FIXED_LUN].capacity.block_nbr == UINT32_MAX))
    {
        return COMMAND_CLASS_INTERFACE;         //fail
    }

    if (MSC_Handle->unit[MSC_FIXED_LUN].capacity.block_size != MSC_SUPPORTED_BLOCK_SIZE)
    {
        return COMMAND_CLASS_INTERFACE;         //fail
    }

    return COMMAND_CLASS_MASS_STORAGE;          //success!
}


void Downstream_MSC_PacketProcessor(DownstreamPacketTypeDef* receivedPacket)
{
    switch (receivedPacket->Command)
    {
    case COMMAND_MSC_TEST_UNIT_READY:
        Downstream_MSC_PacketProcessor_TestUnitReady(receivedPacket);
        break;

    case COMMAND_MSC_GET_CAPACITY:
        Downstream_MSC_PacketProcessor_GetCapacity(receivedPacket);
        break;

    case COMMAND_MSC_READ:
        Downstream_MSC_PacketProcessor_BeginRead(receivedPacket);
        break;

#ifdef CONFIG_MASS_STORAGE_WRITES_PERMITTED
    case COMMAND_MSC_WRITE:
        Downstream_MSC_PacketProcessor_BeginWrite(receivedPacket);
        break;
#endif

    case COMMAND_MSC_DISCONNECT:
        Downstream_MSC_PacketProcessor_Disconnect(receivedPacket);
        break;

    default:
        Downstream_PacketProcessor_FreakOut();
    }

}


static void Downstream_MSC_PacketProcessor_TestUnitReady(DownstreamPacketTypeDef* receivedPacket)
{
    Downstream_ReleasePacket(receivedPacket);

    if (USBH_MSC_UnitIsReady(&hUsbHostFS,
                             MSC_FIXED_LUN,
                             Downstream_MSC_PacketProcessor_TestUnitReadyCallback) != USBH_BUSY)
    {
        Downstream_MSC_PacketProcessor_TestUnitReadyCallback(USBH_FAIL);
    }
}


static void Downstream_MSC_PacketProcessor_TestUnitReadyCallback(USBH_StatusTypeDef result)
{
    DownstreamPacketTypeDef* freePacket;

    freePacket = Downstream_GetFreePacketImmediately();
    freePacket->CommandClass = COMMAND_CLASS_MASS_STORAGE;
    freePacket->Command = COMMAND_MSC_TEST_UNIT_READY;
    freePacket->Length16 = DOWNSTREAM_PACKET_HEADER_LEN_16 + 1;

    if (result == USBH_OK)
    {
        freePacket->Data[0] = HAL_OK;
    }
    else
    {
        freePacket->Data[0] = HAL_ERROR;
    }
    Downstream_PacketProcessor_ClassReply(freePacket);
}



static void Downstream_MSC_PacketProcessor_GetCapacity(DownstreamPacketTypeDef* receivedPacket)
{
    MSC_HandleTypeDef* MSC_Handle =  (MSC_HandleTypeDef*)hUsbHostFS.pActiveClass->pData;

    receivedPacket->Length16 = DOWNSTREAM_PACKET_HEADER_LEN_16 + (8 / 2);
    *(uint32_t*)&(receivedPacket->Data[0]) = MSC_Handle->unit[MSC_FIXED_LUN].capacity.block_nbr;
    *(uint32_t*)&(receivedPacket->Data[4]) = (uint32_t)MSC_Handle->unit[MSC_FIXED_LUN].capacity.block_size;
    Downstream_PacketProcessor_ClassReply(receivedPacket);
}



static void Downstream_MSC_PacketProcessor_BeginRead(DownstreamPacketTypeDef* receivedPacket)
{
    uint64_t readBlockAddress;
    uint32_t readBlockCount;
    uint64_t readByteCount;
    MSC_HandleTypeDef* MSC_Handle =  (MSC_HandleTypeDef*)hUsbHostFS.pActiveClass->pData;

    if (receivedPacket->Length16 != (DOWNSTREAM_PACKET_HEADER_LEN_16 + ((4 * 3) / 2)))
    {
        Downstream_PacketProcessor_FreakOut();
        return;
    }

    readBlockAddress = *(uint64_t*)&(receivedPacket->Data[0]);
    readBlockCount   = *(uint32_t*)&(receivedPacket->Data[8]);
    readByteCount    = readBlockCount * MSC_Handle->unit[MSC_FIXED_LUN].capacity.block_size;
    if ((readBlockAddress                        >= (uint64_t)MSC_Handle->unit[MSC_FIXED_LUN].capacity.block_nbr) ||
        ((readBlockAddress + readBlockCount - 1) >= (uint64_t)MSC_Handle->unit[MSC_FIXED_LUN].capacity.block_nbr) ||
        (readByteCount > UINT32_MAX))
    {
        Downstream_PacketProcessor_FreakOut();
        return;
    }

    receivedPacket->Data[0] = HAL_ERROR;
    receivedPacket->Length16 = DOWNSTREAM_PACKET_HEADER_LEN_16 + 1;
    if (USBH_MSC_Read(&hUsbHostFS,
                      MSC_FIXED_LUN,
                      (uint32_t)readBlockAddress,
                      readBlockCount,
                      Downstream_MSC_PacketProcessor_RdWrCompleteCallback) == USBH_BUSY)
    {
        Downstream_ReleasePacket(receivedPacket);
        return;
    }

    //Fail:
    Downstream_PacketProcessor_ClassReply(receivedPacket);
}


static void Downstream_MSC_PacketProcessor_RdWrCompleteCallback(USBH_StatusTypeDef result)
{
    if (result != USBH_OK)
    {
        Downstream_GetFreePacket(Downstream_PacketProcessor_GenericErrorReply);
        return;
    }
    Downstream_ReceivePacket(Downstream_PacketProcessor);
}



#ifdef CONFIG_MASS_STORAGE_WRITES_PERMITTED
static void Downstream_MSC_PacketProcessor_BeginWrite(DownstreamPacketTypeDef* receivedPacket)
{
    uint64_t writeBlockAddress;
    uint32_t writeBlockCount;
    uint64_t writeByteCount;
    MSC_HandleTypeDef* MSC_Handle =  (MSC_HandleTypeDef*)hUsbHostFS.pActiveClass->pData;

    if (receivedPacket->Length16 != (DOWNSTREAM_PACKET_HEADER_LEN_16 + ((4 * 3) / 2)))
    {
        Downstream_PacketProcessor_FreakOut();
        return;
    }

    writeBlockAddress = *(uint64_t*)&(receivedPacket->Data[0]);
    writeBlockCount   = *(uint32_t*)&(receivedPacket->Data[8]);
    writeByteCount    = writeBlockCount * MSC_Handle->unit[MSC_FIXED_LUN].capacity.block_size;
    if ((writeBlockAddress                         >= (uint64_t)MSC_Handle->unit[MSC_FIXED_LUN].capacity.block_nbr) ||
        ((writeBlockAddress + writeBlockCount - 1) >= (uint64_t)MSC_Handle->unit[MSC_FIXED_LUN].capacity.block_nbr) ||
        (writeByteCount > UINT32_MAX))
    {
        Downstream_PacketProcessor_FreakOut();
        return;
    }

    ReadStreamPacket = NULL;        //Prepare for GetStreamDataPacket's use
    ReadStreamBusy = 0;
    ByteCount = (uint32_t)writeByteCount;

    receivedPacket->Data[0] = HAL_ERROR;
    receivedPacket->Length16 = DOWNSTREAM_PACKET_HEADER_LEN_16 + 1;

    //Our host stack has no way to detect if write-protection is enabled.
    //So currently we can't return HAL_BUSY to Upstream in this situation.
    if (USBH_MSC_Write(&hUsbHostFS,
                       MSC_FIXED_LUN,
                       (uint32_t)writeBlockAddress,
                       writeBlockCount,
                       Downstream_MSC_PacketProcessor_RdWrCompleteCallback) == USBH_BUSY)
    {
        Downstream_ReleasePacket(receivedPacket);
        return;
    }

    //Fail:
    Downstream_PacketProcessor_ClassReply(receivedPacket);
}
#endif


//Used by USB MSC host driver
HAL_StatusTypeDef Downstream_MSC_PutStreamDataPacket(DownstreamPacketTypeDef* packetToSend,
                                                     uint32_t dataLength8)
{
    if ((dataLength8 % 2) != 0)
    {
        return HAL_ERROR;
    }

    packetToSend->Length16 = (dataLength8 / 2) + DOWNSTREAM_PACKET_HEADER_LEN_16;
    packetToSend->CommandClass = COMMAND_CLASS_MASS_STORAGE | COMMAND_CLASS_DATA_FLAG;
    packetToSend->Command = COMMAND_MSC_READ;
    return Downstream_TransmitPacket(packetToSend);
}


#ifdef CONFIG_MASS_STORAGE_WRITES_PERMITTED
//Used by USB MSC host driver
HAL_StatusTypeDef Downstream_MSC_GetStreamDataPacket(DownstreamMSCCallbackPacketTypeDef callback)
{
    GetStreamDataCallback = callback;

    if (ReadStreamBusy != 0)
    {
        return HAL_OK;
    }
    ReadStreamBusy = 1;

    if (ReadStreamPacket && GetStreamDataCallback)      //Do we have a stored packet and an address to send it?
    {
        Downstream_MSC_GetStreamDataPacketCallback(ReadStreamPacket);   //Send it now!
        ReadStreamPacket = NULL;
        return HAL_OK;              //Our callback will call us again, so we don't need to get a packet in this case.
    }
    return Downstream_ReceivePacket(Downstream_MSC_GetStreamDataPacketCallback);
}


void Downstream_MSC_GetStreamDataPacketCallback(DownstreamPacketTypeDef* receivedPacket)
{
    uint16_t dataLength8;

    ReadStreamBusy = 0;
    if (GetStreamDataCallback == NULL)
    {
        ReadStreamPacket = receivedPacket;      //We used up our callback already, so save this one for later.
        return;
    }

    dataLength8 = (receivedPacket->Length16 - DOWNSTREAM_PACKET_HEADER_LEN_16) * 2;

    if ((receivedPacket->CommandClass != (COMMAND_CLASS_MASS_STORAGE | COMMAND_CLASS_DATA_FLAG)) || //Must be MSC command with data flag set
        (receivedPacket->Command != COMMAND_MSC_WRITE) ||                       //Must be write command
        (receivedPacket->Length16 <= DOWNSTREAM_PACKET_HEADER_LEN_16) ||        //Should be at least one data byte in the packet.
        (dataLength8 > ByteCount))
    {
        Downstream_PacketProcessor_FreakOut();
        return;
    }

    ByteCount -= dataLength8;
    GetStreamDataCallback(receivedPacket, dataLength8); //usb_msc_scsi will use this packet, so don't release now
    if (ByteCount > 0)
    {
        Downstream_MSC_GetStreamDataPacket(NULL);   //Try to get the next packet now, before USB asks for it
    }

}
#endif  //#ifdef CONFIG_MASS_STORAGE_WRITES_PERMITTED



static void Downstream_MSC_PacketProcessor_Disconnect(DownstreamPacketTypeDef* receivedPacket)
{
    Downstream_ReleasePacket(receivedPacket);

    USBH_MSC_StartStopUnit(&hUsbHostFS,
                           MSC_FIXED_LUN,
                           MSC_START_STOP_EJECT_FLAG,
                           Downstream_MSC_PacketProcessor_DisconnectCallback);
}



static void Downstream_MSC_PacketProcessor_DisconnectCallback(USBH_StatusTypeDef result)
{
    DownstreamPacketTypeDef* freePacket;

    if (result == USBH_OK)
    {
        freePacket = Downstream_GetFreePacketImmediately();
        freePacket->CommandClass = COMMAND_CLASS_MASS_STORAGE;
        freePacket->Command = COMMAND_MSC_DISCONNECT;
        freePacket->Length16 = DOWNSTREAM_PACKET_HEADER_LEN_16;
        Downstream_PacketProcessor_ClassReply(freePacket);
    }
}

#endif  //#ifdef CONFIG_MASS_STORAGE_ENABLED

