// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

// This sample implements a PnP based Temperature Control model.  It demonstrates a somewhat
// complex model in that the model has properties, commands, and telemetry off of the root component
// as well as subcomponents.  

// PnP is a serialization and deserialization convention built on top of IoTHub DeviceClient
// primitives of telemetry, device methods, and device twins.  A sample helper, ../common/pnp_protocol_helpers.h,
// is used here to perfom the needed (de)serialization.

// The DTDL for component is https://github.com/Azure/opendigitaltwins-dtdl/blob/master/DTDL/v2/samples/TemperatureController.json

// Standard C header files
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// IoTHub Device Client and IoT core utility related header files
#include "iothub.h"
#include "iothub_device_client.h"
#include "iothub_client_options.h"
#include "iothub_message.h"
#include "azure_c_shared_utility/threadapi.h"
#include "azure_c_shared_utility/strings.h"
#include "azure_c_shared_utility/xlogging.h"

// PnP helper utilities.
#include "pnp_device_client_helpers.h"
#include "pnp_protocol_helpers.h"

// Headers that provide implementation for subcomponents (the two thermostat components and DeviceInfo)
#include "pnp_thermostat_component.h"
#include "pnp_deviceinfo_component.h"

// Environment variable used to specify this application's connection string
static const char g_connectionStringEnvironmentVariable[] = "IOTHUB_DEVICE_CONNECTION_STRING";

// Amount of time to sleep between sending telemetry to Hub, in milliseconds.  Set to 1 minute.
static unsigned int g_sleepBetweenTelemetrySends = 60 * 1000;

// Whether tracing at the IoTHub client is enabled or not. 
static bool g_hubClientTraceEnabled = true;

// DTMI indicating this device's ModelId.
static const char g_modelId[] = "dtmi:com:example:TemperatureController;1";

// PNP_THERMOSTAT_COMPONENT_HANDLE represent the thermostat components that are sub-components of the temperature controller.
// Note that we do NOT have an analogous DeviceInfo component handle because the DeviceInfo is so straightforward we skipped that step.
PNP_THERMOSTAT_COMPONENT_HANDLE g_thermostatHandle1;
PNP_THERMOSTAT_COMPONENT_HANDLE g_thermostatHandle2;

// Name of subcomponents that TemmperatureController implements.
static const char g_thermostatComponent1Name[] = "thermostat1";
static const size_t g_thermostatComponent1Size = sizeof(g_thermostatComponent1Name) - 1;
static const char g_thermostatComponent2Name[] = "thermostat2";
static const size_t g_thermostatComponent2Size = sizeof(g_thermostatComponent2Name) - 1;
static const char g_deviceInfoComponentName[] = "deviceInformation";

// Command implemented by the TemperatureControl component itself
static const char g_rebootCommand[] = "reboot";

// Name of json field to parse for reboot's "delay". (Note the "delay" is the only field of the value, "delay" is not explicitly sent)
static const char g_JSONCommandSetting[] = "commandRequest.value";
// An empty JSON body for responses
static const char g_emptyJson[] = "{}";
static const size_t g_emptyJsonSize = sizeof(g_emptyJson) - 1;

// Minimum value we will return for working set, + some random number
static const int g_workingSetMinimum = 1000;
// Random number for working set will range between the g_workingSetMinimum and (g_workingSetMinimum+g_workingSetRandomModule)
static const int g_workingSetRandomModule = 500;
// Format string for sending a telemetry message with the working set.
static const char g_workingSetTelemetryFormat[] = "{\"workingSet\":%d}";

// Name of the serial number property as defined in this component's DTML
static const char g_serialNumberPropertyName[] = "serialNumber";
// Value of the serial number.  NOTE: This must be a legal JSON string which requires value to be in "..."
static const char g_serialNumberPropertyValue[] = "\"serial-no-123-abc\"";

//
// TempControl_InvokeRebootCommand processes the reboot command on the root interface
//
static int TempControl_InvokeRebootCommand(JSON_Object* rootObject, unsigned char** response, size_t* responseSize)
{
    int result;
    JSON_Value* delayValue; 

    if ((delayValue = json_object_dotget_value(rootObject, g_JSONCommandSetting)) == NULL)
    {
        LogError("Payload not specified in command request");
        result = PNP_STATUS_BAD_FORMAT;
    }
    else if (JSONNumber != json_value_get_type(delayValue))
    {
        LogError("Delay payload is not a number");
        result = PNP_STATUS_BAD_FORMAT;
    }
    else
    {
        int delayInSeconds = (int)json_value_get_number(delayValue);
        LogInfo("Temperature controller 'reboot' command invoked with delay=%d seconds", delayInSeconds);

        // Even though the DTMI for TemperatureController does not specify a response body, the underlying IoTHub device method
        // requires a valid JSON to be included in the response.  The SDK will not automatically
        if ((*response = (unsigned char*)malloc(g_emptyJsonSize)) == NULL)
        {
            LogError("Unable to allocate %lu bytes", (unsigned long)(g_emptyJsonSize));
            result = PNP_STATUS_INTERNAL_ERROR;
        }
        else
        {
            // We're using unsigned char**, so do not \0 terminate this string
            memcpy(*response, g_emptyJson, g_emptyJsonSize);
            *responseSize = g_emptyJsonSize;
            result = PNP_STATUS_SUCCESS;
        }
    }
    
    return result;
}

//
// TempControl_DeviceMethodCallback is invoked by IoT SDK when a device method arrives.
//
static int TempControl_DeviceMethodCallback(const char* methodName, const unsigned char* payload, size_t size, unsigned char** response, size_t* responseSize, void* userContextCallback)
{
    (void)userContextCallback;

    char* jsonStr = NULL;
    JSON_Value* rootValue = NULL;
    JSON_Object* rootObject = NULL;
    int result;

    unsigned const char *componentName;
    size_t componentNameSize;

    const char *pnpCommandName;

    *response = NULL;
    *responseSize = 0;

    // Parse the methodName into its PnP (optional) componentName and pnpCommandName.
    PnPHelper_ParseCommandName(methodName, &componentName, &componentNameSize, &pnpCommandName);

    // Parse the JSON of the payload request.
    if ((jsonStr = PnPHelper_CopyPayloadToString(payload, size)) == NULL)
    {
        LogError("Unable to allocate twin buffer");
        result = PNP_STATUS_INTERNAL_ERROR;
    }
    else if ((rootValue = json_parse_string(jsonStr)) == NULL)
    {
        LogError("Unable to parse twin JSON");
        result = PNP_STATUS_BAD_FORMAT;
    }
    else if ((rootObject = json_value_get_object(rootValue)) == NULL)
    {
        LogError("Unable to get root object of JSON");
        result = PNP_STATUS_INTERNAL_ERROR;
    }
    else
    {
        if (componentName != NULL)
        {
            LogInfo("Received PnP command for component=%.*s, command=%s", (int)componentNameSize, componentName, pnpCommandName);
            if (strncmp((const char*)componentName, g_thermostatComponent1Name, g_thermostatComponent1Size) == 0)
            {
                result = PnP_ThermostatComponent_ProcessCommand(g_thermostatHandle1, pnpCommandName, rootObject, response, responseSize);
            }
            else if (strncmp((const char*)componentName, g_thermostatComponent2Name, g_thermostatComponent2Size) == 0)
            {
                result = PnP_ThermostatComponent_ProcessCommand(g_thermostatHandle2, pnpCommandName, rootObject, response, responseSize);
            }
            else
            {
                LogError("PnP component %.*s is not supported by TemperatureController", (int)componentNameSize, componentName);
                result = PNP_STATUS_NOT_FOUND;
            }
        }
        else
        {
            LogInfo("Received PnP command for TemperatureController (root) component, command=%s", pnpCommandName);
            if (strcmp(pnpCommandName, g_rebootCommand) == 0)
            {
                result = TempControl_InvokeRebootCommand(rootObject, response, responseSize);
            }
            else
            {
                LogError("PnP command on root interface %s is not supported by TemperatureController", pnpCommandName);
                result = PNP_STATUS_NOT_FOUND;
            }
        }
    }

    json_value_free(rootValue);
    free(jsonStr);

    return result;
}

//
// TempControl_ApplicationPropertyCallback is the callback function that the PnP helper layer invokes per property update.
//
static void TempControl_ApplicationPropertyCallback(const char* componentName, const char* propertyName, JSON_Value* propertyValue, int version, void* userContextCallback)
{
    // This sample uses the pnp_device_client_helpers.h/.c to create the IOTHUB_DEVICE_CLIENT_HANDLE as well as initialize callbacks.
    // The convention the helper uses is that the IOTHUB_DEVICE_CLIENT_HANDLE is passed as the userContextCallback on the initial twin callback.
    // The pnp_protocol_lehpers.h/.c pass this userContextCallback down to this visitor function.
    IOTHUB_DEVICE_CLIENT_HANDLE deviceClient = (IOTHUB_DEVICE_CLIENT_HANDLE)userContextCallback;

    if (componentName == NULL)
    {
        LogError("Property %s arrived for TemperatureControl component itself.  This does not support writeable properties on it directly (all properties are on subcomponents)", propertyName);
    }
    else if (strcmp(componentName, g_thermostatComponent1Name) == 0)
    {
        PnP_ThermostatComponent_ProcessPropertyUpdate(g_thermostatHandle1, deviceClient, propertyName, propertyValue, version);
    }
    else if (strcmp(componentName, g_thermostatComponent1Name) == 0)
    {
        PnP_ThermostatComponent_ProcessPropertyUpdate(g_thermostatHandle1, deviceClient, propertyName, propertyValue, version);
    }
    else
    {
        // The PnP protocol does not define a mechanism to report errors such as this to IoTHub, so 
        // the best we can do here is to log for diagnostics purposes.
        LogError("Component %s is not implemented by the ThermostatController", componentName);
    }
}

//
// TempControl_DeviceTwinCallback is invoked by IoT SDK when a twin - either full twin or a PATCH update - arrives.
//
static void TempControl_DeviceTwinCallback(DEVICE_TWIN_UPDATE_STATE updateState, const unsigned char* payload, size_t size, void* userContextCallback)
{
    // Invoke PnPHelper_ProcessTwinData to actualy process the data.  PnPHelper_ProcessTwinData uses a visitor pattern to parse
    // the JSON and then visit each property, invoking TempControl_ApplicationPropertyCallback on each element.
    if (PnPHelper_ProcessTwinData(updateState, payload, size, TempControl_ApplicationPropertyCallback, userContextCallback) == false)
    {
        // If we're unable to parse the JSON for any reason (typically because the JSON is malformed or we ran out of memory)
        // there is no actiol we can take beyond logging.
        LogError("Unable to process twin json.  Ignoring any desired property update requests");
    }
}

//
// TempControl_SendWorkingSet sends a PnP telemetry indicating the current working set of the device, in 
// the unit of kibibytes (https://en.wikipedia.org/wiki/Kibibyte).
// This is a random value between g_workingSetMinimum and (g_workingSetMinimum+g_workingSetRandomModule).
//
void TempControl_SendWorkingSet(IOTHUB_DEVICE_CLIENT_HANDLE deviceClient) 
{
    IOTHUB_MESSAGE_HANDLE messageHandle = NULL;
    IOTHUB_CLIENT_RESULT iothubResult;
    char workingSetTelemetryPayload[64];

    int workingSet = g_workingSetMinimum + (rand() % g_workingSetRandomModule);

    if (snprintf(workingSetTelemetryPayload, sizeof(workingSetTelemetryPayload), g_workingSetTelemetryFormat, workingSet) < 0)
    {
        LogError("Unable to create a workingSet telemetry payload string");
    }
    else if ((messageHandle = PnPHelper_CreateTelemetryMessageHandle(NULL, workingSetTelemetryPayload)) == NULL)
    {
        LogError("Unable to create telemetry message");
    }
    else if ((iothubResult = IoTHubDeviceClient_SendEventAsync(deviceClient, messageHandle, NULL, NULL)) != IOTHUB_CLIENT_OK)
    {
        LogError("Unable to send telemetry message, error=%d", iothubResult);
    }

    IoTHubMessage_Destroy(messageHandle);
}

//
// TempControl_ReportSerialNumberProperty sends the "serialNumber" property to IoTHub
//
static void TempControl_ReportSerialNumberProperty(IOTHUB_DEVICE_CLIENT_HANDLE deviceClient)
{
    IOTHUB_CLIENT_RESULT iothubClientResult;
    STRING_HANDLE jsonToSend = NULL;

    if ((jsonToSend = PnPHelper_CreateReportedProperty(NULL, g_serialNumberPropertyName, g_serialNumberPropertyValue)) == NULL)
    {
        LogError("Unable to build serial number property");
    }
    else
    {
        const char* jsonToSendStr = STRING_c_str(jsonToSend);
        size_t jsonToSendStrLen = strlen(jsonToSendStr);

        if ((iothubClientResult = IoTHubDeviceClient_SendReportedState(deviceClient, (const unsigned char*)jsonToSendStr, jsonToSendStrLen, NULL, NULL)) != IOTHUB_CLIENT_OK)
        {
            LogError("Unable to send reported state.  Error=%d", iothubClientResult);
        }
        else
        {
            LogInfo("Sending serialNumber property to IoTHub");
        }
    }

    STRING_delete(jsonToSend);
}

int main(void)
{
    IOTHUB_DEVICE_CLIENT_HANDLE deviceClient = NULL;
    const char* connectionString;

    // Invoke random numbers with the same seed instead of time based so that results are reproducible
    srand(1);

    if ((connectionString = getenv(g_connectionStringEnvironmentVariable)) == NULL)
    {
        LogError("Cannot read environment variable %s", g_connectionStringEnvironmentVariable);
    }
    else if ((deviceClient = PnPHelper_CreateDeviceClientHandle(connectionString, g_modelId, g_hubClientTraceEnabled, TempControl_DeviceMethodCallback, TempControl_DeviceTwinCallback)) == NULL)
    {
        LogError("Failure creating IotHub device client");
    }
    else if ((g_thermostatHandle1 = PnP_ThermostatComponent_CreateHandle(g_thermostatComponent1Name)) == NULL)
    {
        LogError("Unable to create component handle for %s", g_thermostatComponent1Name);
    }
    else if ((g_thermostatHandle2 = PnP_ThermostatComponent_CreateHandle(g_thermostatComponent2Name)) == NULL)
    {
        LogError("Unable to create component handle for %s", g_thermostatComponent2Name);
    }
    else
    {
        LogInfo("Successfully created device client and thermostat component handle.  Hit Control-C to exit program\n");

        TempControl_ReportSerialNumberProperty(deviceClient);
        // Report DeviceInfo properties about this simulated device.
        PnP_DeviceInfoComponent_ReportInfo(deviceClient, g_deviceInfoComponentName);

        while (true)
        {
            // Wake up periodically to send telemetry.
            // IOTHUB_DEVICE_CLIENT_HANDLE brings in the IoTHub device convenience layer, which means that the IoTHub SDK itself 
            // spins a worker thread to perform all operations.
            TempControl_SendWorkingSet(deviceClient);
            PnP_ThermostatComponent_SendTelemetry(g_thermostatHandle1, deviceClient);
            PnP_ThermostatComponent_SendTelemetry(g_thermostatHandle2, deviceClient);
            ThreadAPI_Sleep(g_sleepBetweenTelemetrySends);
        }

        // Free the memory allocated to track simulated thermostat.
        PnP_ThermostatComponent_Destroy(g_thermostatHandle2);
        PnP_ThermostatComponent_Destroy(g_thermostatHandle1);

        // Clean up the iothub sdk handle
        IoTHubDeviceClient_Destroy(deviceClient);
        // Free all the sdk subsystem
        IoTHub_Deinit();        
    }

    return 0;
}

