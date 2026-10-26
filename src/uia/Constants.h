// define necessary constants here if UIAutomationClient.h is not available

// Providers are created on the OleInitialize STA in SumatraStartup. UIA must
// marshal callbacks to that apartment, where tab/model lifetime is managed.
// Without this flag it calls on worker threads, racing FreeDocument and even
// bypassing the released check while a text range is being constructed.
constexpr ProviderOptions kSumatraUiaProviderOptions =
    (ProviderOptions)(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);

#ifndef __UIA_PatternIds_MODULE_DEFINED__
const long UIA_TextPatternId = 10014;
const long UIA_ValuePatternId = 10002;
#endif //__UIA_PatternIds_MODULE_DEFINED__

#ifndef __UIA_EventIds_MODULE_DEFINED__
const long UIA_Text_TextSelectionChangedEventId = 20014;
#endif //__UIA_EventIds_MODULE_DEFINED__

#ifndef __UIA_PropertyIds_MODULE_DEFINED__
const long UIA_NamePropertyId = 30005;
const long UIA_ControlTypePropertyId = 30003;
const long UIA_IsKeyboardFocusablePropertyId = 30009;
const long UIA_AutomationIdPropertyId = 30011;
const long UIA_IsControlElementPropertyId = 30016;
const long UIA_IsContentElementPropertyId = 30017;
const long UIA_NativeWindowHandlePropertyId = 30020;
const long UIA_IsTextPatternAvailablePropertyId = 30040;
const long UIA_IsValuePatternAvailablePropertyId = 30043;
#endif //__UIA_PropertyIds_MODULE_DEFINED__

#ifndef __UIA_ControlTypeIds_MODULE_DEFINED__
const long UIA_CustomControlTypeId = 50025;
const long UIA_DocumentControlTypeId = 50030;
#endif //__UIA_ControlTypeIds_MODULE_DEFINED__
