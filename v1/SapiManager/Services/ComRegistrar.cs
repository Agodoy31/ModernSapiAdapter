using System;
using System.Runtime.InteropServices;

namespace SapiManager.Services;

public static class ComRegistrar
{
    private const uint LOAD_WITH_ALTERED_SEARCH_PATH = 0x00000008;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr LoadLibraryEx(string lpFileName, IntPtr hFile, uint dwFlags);

    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true, ExactSpelling = true)]
    private static extern IntPtr GetProcAddress(IntPtr hModule, string procName);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool FreeLibrary(IntPtr hModule);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int DllRegisterServerDelegate();

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int DllUnregisterServerDelegate();

    /// <summary>
    /// Calls DllRegisterServer on the specified COM DLL path.
    /// </summary>
    public static bool RegisterComDll(string dllPath)
    {
        IntPtr hModule = LoadLibraryEx(dllPath, IntPtr.Zero, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (hModule == IntPtr.Zero)
        {
            return false;
        }

        try
        {
            IntPtr pFunc = GetProcAddress(hModule, "DllRegisterServer");
            if (pFunc == IntPtr.Zero) return false;

            var registerFunc = Marshal.GetDelegateForFunctionPointer<DllRegisterServerDelegate>(pFunc);
            int hr = registerFunc();
            return hr >= 0; // S_OK / S_FALSE
        }
        catch
        {
            return false;
        }
        finally
        {
            FreeLibrary(hModule);
        }
    }

    /// <summary>
    /// Calls DllUnregisterServer on the specified COM DLL path.
    /// </summary>
    public static bool UnregisterComDll(string dllPath)
    {
        IntPtr hModule = LoadLibraryEx(dllPath, IntPtr.Zero, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (hModule == IntPtr.Zero)
        {
            return false;
        }

        try
        {
            IntPtr pFunc = GetProcAddress(hModule, "DllUnregisterServer");
            if (pFunc == IntPtr.Zero) return false;

            var unregisterFunc = Marshal.GetDelegateForFunctionPointer<DllUnregisterServerDelegate>(pFunc);
            int hr = unregisterFunc();
            return hr >= 0;
        }
        catch
        {
            return false;
        }
        finally
        {
            FreeLibrary(hModule);
        }
    }

    [UnmanagedFunctionPointer(CallingConvention.StdCall, CharSet = CharSet.Unicode)]
    private delegate bool ProviderGenerateManifestDelegate([MarshalAs(UnmanagedType.LPWStr)] string outputDir);

    /// <summary>
    /// Invokes ProviderGenerateManifest on the specified provider DLL.
    /// </summary>
    public static bool GenerateProviderManifest(string dllPath, string outputDir)
    {
        IntPtr hModule = LoadLibraryEx(dllPath, IntPtr.Zero, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (hModule == IntPtr.Zero)
        {
            return false;
        }

        try
        {
            IntPtr pFunc = GetProcAddress(hModule, "ProviderGenerateManifest");
            if (pFunc == IntPtr.Zero) return false;

            var generateFunc = Marshal.GetDelegateForFunctionPointer<ProviderGenerateManifestDelegate>(pFunc);
            return generateFunc(outputDir);
        }
        catch
        {
            return false;
        }
        finally
        {
            FreeLibrary(hModule);
        }
    }
}
