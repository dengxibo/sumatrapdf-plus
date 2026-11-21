# Online speech configuration

In the read-aloud settings, open **Online setup…** and enter your provider details. Click **OK**, choose **Online speech**, select the provider, and click **Preview** to try it. The app encrypts the entire online speech configuration with Windows DPAPI and saves it as `ReadAloudOnlineSpeechConfig` in the active `SumatraPDF-settings.txt`. A new settings file starts with empty online speech fields. Re-enter values that were saved by an older version in the Windows registry.

## Microsoft Azure Speech

Fill in the Azure region (for example `eastasia`), resource key and voice (for example `zh-CN-YunxiNeural`).

Select **Microsoft Azure Speech**. The app calls the regional endpoint in the public Azure cloud. Azure receives the text of each chunk you read.

## JoyInside

In **Online setup…**, enter your JoyInside AccessKey, SecretKey and the `botId` of a device registered in your JoyInside account. Select **JoyInside · Mature uncle** under **Online speech**. SumatraPDF Plus uses voice ID `40486` and connects directly over HTTPS and WebSocket.

To load your organization's available voices, also enter its JoyInside enterprise ID (`vendorId`). The app loads that catalog in the background and lists the returned voices before the Edge voices. The enterprise ID is optional for the preset voice.

The SecretKey is encrypted for your Windows account using DPAPI. Register the device in JoyInside before entering its ID. The device and credentials must have access to this voice.

These interfaces return audio without word timing. They support chunk playback, with Azure rate set through SSML and JoyInside rate set by the service.
