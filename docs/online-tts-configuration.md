# Online speech configuration

In the read-aloud settings, open **Online setup…** and enter your provider details. Click **OK**, choose **Online speech**, select the provider, and click **Preview** to try it. The setup dialog saves values for the current Windows user and encrypts the Azure key with Windows DPAPI. Environment variables remain available as defaults before a field is saved in the dialog.

## Microsoft Azure Speech

Fill in the Azure region, resource key, and voice. For launches configured through the environment, the equivalent variables are:

| Variable | Value |
| --- | --- |
| `SUMATRA_AZURE_REGION` | The region of your Azure Speech resource, for example `eastasia` |
| `SUMATRA_AZURE_KEY` | Your Speech resource key |
| `SUMATRA_AZURE_VOICE` | A standard Azure voice name, for example `zh-CN-YunxiNeural` |

Select **Microsoft Azure Speech**. The app calls the regional endpoint in the public Azure cloud. Azure receives the text of each chunk you read. The resource key stays out of SumatraPDF settings and the installer.

## JoyInside

In **Online setup…**, enter your JoyInside AccessKey, SecretKey and the `botId` of a device registered in your JoyInside account. Select **JoyInside · Mature uncle** under **Online speech**. SumatraPDF Plus uses voice ID `40486` and connects directly over HTTPS and WebSocket.

To load your organization's available voices, also enter its JoyInside enterprise ID (`vendorId`). The app loads that catalog in the background and lists the returned voices before the Edge voices. The enterprise ID is optional for the preset voice.

The SecretKey is encrypted for your Windows account using DPAPI. Register the device in JoyInside before entering its ID. The device and credentials must have access to this voice.

These interfaces return audio without word timing. They support chunk playback, with Azure rate set through SSML and JoyInside rate set by the service.
