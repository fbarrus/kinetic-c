/*
* kinetic-c
* Copyright (C) 2014 Seagate Technology.
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*
*/

#include "kinetic_pdu.h"
#include "kinetic_nbo.h"
#include "kinetic_connection.h"
#include "kinetic_socket.h"
#include "kinetic_hmac.h"
#include "kinetic_logger.h"
#include "kinetic_proto.h"

void KineticPDU_Init(KineticPDU* const pdu,
    KineticConnection* const connection)
{
    KINETIC_PDU_INIT(pdu, connection);
}

void KineticPDU_AttachValuePayload(KineticPDU* const pdu,
    ByteArray payload)
{
    assert(pdu != NULL);
    pdu->value = payload;
}

void KineticPDU_EnableValueBuffer(KineticPDU* const pdu)
{
    assert(pdu != NULL);
    pdu->value = {.data = pdu->valueBuffer};
}

void KineticPDU_EnableValueBufferWithLength(KineticPDU* const pdu,
    size_t length)
{
    assert(pdu != NULL);
    assert(length <= sizeof(pdu->valueBuffer));
    pdu->value = (ByteArray){.data = pdu->valueBuffer, .len = length};
}

KineticProto_Status_StatusCode KineticPDU_Status(KineticPDU* const pdu)
{
    assert(pdu != NULL);

    KineticProto_Status_StatusCode status = KINETIC_PROTO_STATUS_STATUS_CODE_INVALID_STATUS_CODE;

    if (pdu->proto)
    {
        if (pdu->proto->command && pdu->proto->command->status)
        {
            status = pdu->proto->command->status->code;
        }
    }
    else if (&pdu->message->command && pdu->message->command.status)
    {
        status = pdu->message->command.status->code;
    }

    return status;
}

bool KineticPDU_Send(KineticPDU* const request)
{
    assert(request != NULL);
    assert(request->connection != NULL);
    assert(request->message != NULL);

    int fd = request->connection->socketDescriptor;

    // Populate the HMAC for the protobuf
    KineticHMAC_Init(&request->hmac, KINETIC_PROTO_SECURITY_ACL_HMACALGORITHM_HmacSHA1);
    KineticHMAC_Populate(&request->hmac,
        &request->message->proto, request->connection->key);

    // Configure PDU header length fields
    request->header.versionPrefix = 'F';
    request->header.protobufLength =
        KineticProto__get_packed_size(&request->message->proto);
    request->header.valueLength = request->value.len;
    KineticLogger_LogHeader(&request->header);

    // Create NBO copy of header for sending
    request->headerNBO.versionPrefix = request->header.versionPrefix;
    request->headerNBO.protobufLength =
        KineticNBO_FromHostU32(request->header.protobufLength);
    request->headerNBO.valueLength =
        KineticNBO_FromHostU32(request->header.valueLength);

    // Pack and send the PDU header
    ByteArray headerNBO = {
        .data = (uint8_t*)&request->headerNBO,
        .len = sizeof(KineticPDUHeader)
    };
    if (!KineticSocket_Write(request->connection->socketDescriptor,
        headerNBO))
    {
        LOG("Failed to send PDU header!");
        return false;
    }

    // Send the protobuf message
    LOG("Sending PDU Protobuf:");
    KineticLogger_LogProtobuf(&request->message->proto);
    if (!KineticSocket_WriteProtobuf(request->connection->socketDescriptor,
        &request->message->proto))
    {
        LOG("Failed to send PDU protobuf message!");
        return false;
    }

    // Send the value/payload, if specified
    if ((request->value.len > 0) && (request->value.data != NULL))
    {
        if (!KineticSocket_Write(request->connection->socketDescriptor,
            request->value))
        {
            LOG("Failed to send PDU value payload!");
            return false;
        }
    }

    return true;
}

bool KineticPDU_Receive(KineticPDU* const response)
{
    const int fd = response->connection->socketDescriptor;
    LOGF("Attempting to receive PDU via fd=%d", fd);
    assert(fd >= 0);
    assert(response != NULL);
    ByteArray rawHeader = {
        .data = (uint8_t*)&response->headerNBO,
        .len = sizeof(KineticPDUHeader) };

    // Receive the PDU header
    if (!KineticSocket_Read(fd, rawHeader))
    {
        LOG("Failed to receive PDU header!");
        return false;
    }
    else
    {
        LOG("PDU header received successfully");
        response->header = (KineticPDUHeader)
        {
            .versionPrefix = response->headerNBO.versionPrefix,
            .protobufLength = 
                KineticNBO_ToHostU32(response->headerNBO.protobufLength),
            .valueLength =
                KineticNBO_ToHostU32(response->headerNBO.valueLength)
        };
        response->value.len = response->header.valueLength;
        KineticLogger_LogHeader(&response->header);
    }

    // Receive the protobuf message
    ByteArray protobuf = {
        .data = response->protobufRaw,
        .len = response->header.protobufLength
    };
    response->proto = (KineticProto*)protobuf.data;
    response->protobufLength = protobuf.len;
    if (!KineticSocket_ReadProtobuf(fd,
            &response->proto,
            protobuf))
    {
        LOG("Failed to receive PDU protobuf message!");
        return false;
    }
    else
    {
        LOG("Received PDU protobuf");
        KineticLogger_LogProtobuf(response->proto);
    }

    // Validate the HMAC for the recevied protobuf message
    if (!KineticHMAC_Validate(response->proto, response->connection->key))
    {
        LOG("Received PDU protobuf message has invalid HMAC!");
        response->proto->command->status->code =
            KINETIC_PROTO_STATUS_STATUS_CODE_DATA_ERROR;

        // FIXME!! success = false;
        // .. but allow HMAC validation to report the error
        // return false;
        // Return true for now until HMAC validation is fixed!
        return true;
    }
    else
    {
        LOG("Received protobuf HMAC validation succeeded");
    }

    // Receive the value payload, if specified
    if (response->header.valueLength > 0)
    {
        assert(response->value.data != NULL);

        LOGF("Attempting to receive value payload (%lld bytes)...",
            (long long)response->header.valueLength);

        if (!KineticSocket_Read(fd, response->value))
        {
            LOG("Failed to receive PDU value payload!");
            return false;
        }
        else
        {
            LOG("Received value payload successfully");
            KineticLogger_LogByteArray("Value Payload", response->value);
        }
    }

    return true;
}
