using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using System.Text;

namespace SapiManager.Services;

public static class ShortcutManager
{
    [ComImport]
    [Guid("00021401-0000-0000-C000-000000000046")]
    private class ShellLink { }

    [ComImport]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    [Guid("000214F9-0000-0000-C000-000000000046")]
    private interface IShellLinkW
    {
        void GetPath([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszFile, int cchMaxPath, out IntPtr pfd, uint fFlags);
        void GetIDList(out IntPtr ppidl);
        void SetIDList(IntPtr pidl);
        void GetDescription([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszName, int cchMaxBuf);
        void SetDescription([MarshalAs(UnmanagedType.LPWStr)] string pszName);
        void GetWorkingDirectory([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszDir, int cchMaxBuf);
        void SetWorkingDirectory([MarshalAs(UnmanagedType.LPWStr)] string pszDir);
        void GetArguments([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszArgs, int cchMaxBuf);
        void SetArguments([MarshalAs(UnmanagedType.LPWStr)] string pszArgs);
        void GetHotkey(out ushort pwHotkey);
        void SetHotkey(ushort wHotkey);
        void GetShowCmd(out int piShowCmd);
        void SetShowCmd(int iShowCmd);
        void Resolve(IntPtr hwnd, uint fFlags);
        void SetPath([MarshalAs(UnmanagedType.LPWStr)] string pszFile);
    }

    /// <summary>
    /// Creates a Start Menu shortcut for ModernSapiAdapter Speech Manager in C:\ProgramData\Microsoft\Windows\Start Menu\Programs.
    /// </summary>
    public static bool CreateStartMenuShortcut(string targetExePath)
    {
        try
        {
            string startMenuFolder = Environment.GetFolderPath(Environment.SpecialFolder.CommonPrograms);
            string shortcutPath = Path.Combine(startMenuFolder, "ModernSapiAdapter Speech Manager.lnk");

            var link = (IShellLinkW)new ShellLink();
            link.SetPath(targetExePath);
            link.SetWorkingDirectory(Path.GetDirectoryName(targetExePath) ?? string.Empty);
            link.SetDescription("ModernSapiAdapter SAPI 5 Speech Manager");

            var file = (IPersistFile)link;
            file.Save(shortcutPath, false);
            return true;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"Failed to create Start Menu shortcut: {ex.Message}");
            return false;
        }
    }

    /// <summary>
    /// Deletes the Start Menu shortcut during uninstallation.
    /// </summary>
    public static bool RemoveStartMenuShortcut()
    {
        try
        {
            string startMenuFolder = Environment.GetFolderPath(Environment.SpecialFolder.CommonPrograms);
            string shortcutPath = Path.Combine(startMenuFolder, "ModernSapiAdapter Speech Manager.lnk");
            if (File.Exists(shortcutPath))
            {
                File.Delete(shortcutPath);
            }
            return true;
        }
        catch
        {
            return false;
        }
    }
}
