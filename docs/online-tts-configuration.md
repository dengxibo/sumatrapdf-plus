# Online speech configuration

In the read-aloud settings, open **Online setup…** and enter your provider details. Click **OK**, choose **Online speech**, select the provider, and click **Preview** to try it. The app encrypts the entire online speech configuration with Windows DPAPI and saves it as `ReadAloudOnlineSpeechConfig` in the active `SumatraPDF-settings.txt`. A new settings file starts with empty online speech fields. Re-enter values that were saved by an older version in the Windows registry.

## Microsoft Azure Speech

Fill in the Azure region (for example `eastasia`), resource key and voice (for example `zh-CN-YunxiNeural`).

Select **Microsoft Azure Speech**. The app calls the regional endpoint in the public Azure cloud. Azure receives the text of each chunk you read.

## JoyInside

In **Online setup…**, enter your JoyInside AccessKey, SecretKey and the `botId` of a device registered in your JoyInside account. Under **Online speech**, select **JoyInside · 沉稳男声** (formerly 唐僧, voice ID `40486`) or **JoyInside · 温情女声** (formerly F25 青年-温情范, voice ID `40245`). SumatraPDF Plus connects directly over HTTPS and WebSocket.

Only these two voices are listed. No enterprise catalog request or enterprise ID (`vendorId`) is required. Previously saved enterprise IDs remain compatible with the encrypted settings format.

The SecretKey is encrypted for your Windows account using DPAPI. Register the device in JoyInside before entering its ID. The device and credentials must have access to this voice.

These interfaces return audio without word timing. They support chunk playback, with Azure rate set through SSML and JoyInside rate set by the service.

JoyInside reuses an active connection for consecutive chunks and updates its configuration when the voice or speed changes. After 15 seconds of inactivity it reconnects; after 60 seconds it also refreshes the cached authentication token when opening a connection. These extra round trips make a cold start slower than continuous playback. Cancelling synthesis closes its WebSocket so an old request cannot keep the next voice waiting on a blocked receive. Diagnostic logs separate lock wait, authentication, connection readiness and first audio latency.
