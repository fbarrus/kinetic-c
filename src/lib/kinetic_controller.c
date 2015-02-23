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
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*
*/

#include "kinetic_controller.h"
#include "kinetic_session.h"
#include "kinetic_operation.h"
#include "kinetic_pdu.h"
#include "kinetic_auth.h"
#include "kinetic_socket.h"
#include "kinetic_allocator.h"
#include "kinetic_resourcewaiter.h"
#include "kinetic_logger.h"
#include <pthread.h>
#include "bus.h"

KineticOperation* KineticController_CreateOperation(KineticSession const * const session)
{
    if (session == NULL) {
        LOG0("Specified session is NULL");
        return NULL;
    }

    if (session->connection == NULL) {
        LOG0("Specified session is not associated with a connection");
        return NULL;
    }

    LOGF3("--------------------------------------------------\n"
         "Building new operation on session @ 0x%llX", session);

    KineticOperation* operation = KineticAllocator_NewOperation(session->connection);
    if (operation == NULL || operation->request == NULL) {
        return NULL;
    }

    return operation;
}

typedef struct {
    pthread_mutex_t receiveCompleteMutex;
    pthread_cond_t receiveComplete;
    bool completed;
    KineticStatus status;
} DefaultCallbackData;

static void DefaultCallback(KineticCompletionData* kinetic_data, void* client_data)
{
    DefaultCallbackData * data = client_data;
    pthread_mutex_lock(&data->receiveCompleteMutex);
    data->status = kinetic_data->status;
    data->completed = true;
    pthread_cond_signal(&data->receiveComplete);
    pthread_mutex_unlock(&data->receiveCompleteMutex);
}

static KineticCompletionClosure DefaultClosure(DefaultCallbackData * const data)
{
    return (KineticCompletionClosure) {
        .callback = DefaultCallback,
        .clientData = data,
    };
}

KineticStatus KineticController_ExecuteOperation(KineticOperation* operation, KineticCompletionClosure* const closure)
{
    KINETIC_ASSERT(operation != NULL);
    KINETIC_ASSERT(operation->connection != NULL);
    KINETIC_ASSERT(operation->connection->pSession != NULL);
    KineticStatus status = KINETIC_STATUS_INVALID;

    if (closure != NULL)
    {
        operation->closure = *closure;
        return KineticOperation_SendRequest(operation);
    }
    else
    {
        DefaultCallbackData data;
        pthread_mutex_init(&data.receiveCompleteMutex, NULL);
        pthread_cond_init(&data.receiveComplete, NULL);
        data.status = KINETIC_STATUS_INVALID;
        data.completed = false;

        operation->closure = DefaultClosure(&data);

        // Send the request
        status = KineticOperation_SendRequest(operation);

        if (status == KINETIC_STATUS_SUCCESS) {
            pthread_mutex_lock(&data.receiveCompleteMutex);
            while(data.completed == false)
            { pthread_cond_wait(&data.receiveComplete, &data.receiveCompleteMutex); }
            status = data.status;
            pthread_mutex_unlock(&data.receiveCompleteMutex);
        }

        pthread_cond_destroy(&data.receiveComplete);
        pthread_mutex_destroy(&data.receiveCompleteMutex);

        return status;
    }
}

KineticStatus bus_to_kinetic_status(bus_send_status_t const status)
{
    KineticStatus res = KINETIC_STATUS_INVALID;

    switch(status)
    {
        // TODO scrutinize all these mappings
        case BUS_SEND_SUCCESS:
            res = KINETIC_STATUS_SUCCESS;
            break;
        case BUS_SEND_TX_TIMEOUT:
            res = KINETIC_STATUS_SOCKET_TIMEOUT;
            break;
        case BUS_SEND_TX_FAILURE:
            res = KINETIC_STATUS_SOCKET_ERROR;
            break;
        case BUS_SEND_RX_TIMEOUT:
            res = KINETIC_STATUS_OPERATION_TIMEDOUT;
            break;
        case BUS_SEND_RX_FAILURE:
            res = KINETIC_STATUS_SOCKET_ERROR;
            break;
        case BUS_SEND_BAD_RESPONSE:
            res = KINETIC_STATUS_SOCKET_ERROR;
            break;
        case BUS_SEND_UNREGISTERED_SOCKET:
            res = KINETIC_STATUS_SOCKET_ERROR;
            break;
        case BUS_SEND_RX_TIMEOUT_EXPECT:
            res = KINETIC_STATUS_OPERATION_TIMEDOUT;
            break;
        case BUS_SEND_UNDEFINED:
        default:
        {
            LOGF0("bus_to_kinetic_status: UNMATCHED %d\n", status);
            KINETIC_ASSERT(false);
            return KINETIC_STATUS_INVALID;
        }
    }
    
    LOGF3("bus_to_kinetic_status: mapping status %d => %d\n",
        status, res);
    return res;
}

static const char *bus_error_string(bus_send_status_t t) {
    switch (t) {
    default:
    case BUS_SEND_UNDEFINED:
        return "undefined";
    case BUS_SEND_SUCCESS:
        return "success";
    case BUS_SEND_TX_TIMEOUT:
        return "tx_timeout";
    case BUS_SEND_TX_FAILURE:
        return "tx_failure";
    case BUS_SEND_RX_TIMEOUT:
        return "rx_timeout";
    case BUS_SEND_RX_FAILURE:
        return "rx_failure";
    case BUS_SEND_BAD_RESPONSE:
        return "bad_response";
    case BUS_SEND_UNREGISTERED_SOCKET:
        return "unregistered socket";
    case BUS_SEND_RX_TIMEOUT_EXPECT:
        return "internal timeout";
    }
}

void KineticController_HandleUnexpectedResponse(void *msg,
                                                int64_t seq_id,
                                                void *bus_udata,
                                                void *socket_udata)
{
    KineticResponse * response = msg;
    KineticConnection* connection = socket_udata;
    bool connetionInfoReceived = false;
    char const * statusTag = "[PDU RX STATUS]";
    char const * unexpectedTag = "[PDU RX UNEXPECTED]";
    char const * logTag = unexpectedTag;
    int logAtLevel, protoLogAtLevel;

    (void)bus_udata;

    // Handle unsolicited status PDUs
    if (response->proto->authType == KINETIC_PROTO_MESSAGE_AUTH_TYPE_UNSOLICITEDSTATUS) {
        if (response->command != NULL &&
            response->command->header != NULL &&
            response->command->header->has_connectionID)
        {
            // Extract connectionID from unsolicited status message
            connection->connectionID = response->command->header->connectionID;
            LOGF2("Extracted connection ID from unsolicited status PDU (id=%lld)",
                connection->connectionID);
            connetionInfoReceived = true;
            logTag = statusTag;
            logAtLevel = 2;
            protoLogAtLevel = 2;
        }
        else {
            LOG0("WARNING: Unsolicited status received. Connection being terminated by remote!");
            logTag = statusTag;
            logAtLevel = 0; 
            protoLogAtLevel = 0;
        }
    }
    else
    {
        LOG0("WARNING: Received unexpected response!");
        logTag = unexpectedTag;
        logAtLevel = 0;
        protoLogAtLevel = 0;
    }

    KineticLogger_LogPrintf(logAtLevel, "%s pdu: %p, session: %p, bus: %p, "
        "fd: %6d, seq: %8lld, protoLen: %8u, valueLen: %8u",
        logTag,
        (void*)response, (void*)connection->pSession,
        (void*)connection->messageBus,
        connection->socket, (long long)seq_id,
        response->header.protobufLength, response->header.valueLength);
    KineticLogger_LogProtobuf(protoLogAtLevel, response->proto);

    KineticAllocator_FreeKineticResponse(response);

    if (connetionInfoReceived) {
        KineticResourceWaiter_SetAvailable(&connection->connectionReady);
    }
}

void KineticController_HandleResult(bus_msg_result_t *res, void *udata)
{
    KineticOperation* op = udata;
    KINETIC_ASSERT(op);
    KINETIC_ASSERT(op->connection);

    KineticStatus status = bus_to_kinetic_status(res->status);

    if (status == KINETIC_STATUS_SUCCESS) {
        KineticResponse * response = res->u.response.opaque_msg;
        KINETIC_ASSERT(response);
        KINETIC_ASSERT(response->command);
        KINETIC_ASSERT(response->command->header);

        if (response->command != NULL &&
            response->command->status != NULL &&
            response->command->status->has_code)
        {
            status = KineticProtoStatusCode_to_KineticStatus(response->command->status->code);
            op->response = response;
        }
        else {
            status = KINETIC_STATUS_INVALID;
        }

        LOGF2("[PDU RX] pdu: %p, session: %p, bus: %p, "
            "fd: %6d, seq: %8lld, protoLen: %8u, valueLen: %8u, op: %p, status: %s",
            (void*)response,
            (void*)op->connection->pSession, (void*)op->connection->messageBus,
            op->connection->socket, response->command->header->ackSequence,
            response->header.protobufLength, response->header.valueLength,
            (void*)op,
            Kinetic_GetStatusDescription(status));
        KineticLogger_LogHeader(3, &response->header);
        KineticLogger_LogProtobuf(3, response->proto);
    }
    else
    {
        // pull out bus error?
        LOGF0("Error receiving response, got message bus error: %s", bus_error_string(res->status));
    }

    // Call operation-specific callback, if configured
    if (op->callback != NULL) {
        status = op->callback(op, status);
    }

    KineticOperation_Complete(op, status);
}

