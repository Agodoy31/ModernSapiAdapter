using System;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

namespace SapiManager.Services;

public static class CredentialManager
{
    private const int CRED_TYPE_GENERIC = 1;
    private const int CRED_PERSIST_LOCAL_MACHINE = 2;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct CREDENTIAL
    {
        public int Flags;
        public int Type;
        public string TargetName;
        public string Comment;
        public System.Runtime.InteropServices.ComTypes.FILETIME LastWritten;
        public int CredentialBlobSize;
        public IntPtr CredentialBlob;
        public int Persist;
        public int AttributeCount;
        public IntPtr Attributes;
        public string TargetAlias;
        public string UserName;
    }

    [DllImport("advapi32.dll", EntryPoint = "CredWriteW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CredWrite([In] ref CREDENTIAL userCredential, [In] uint flags);

    [DllImport("advapi32.dll", EntryPoint = "CredReadW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CredRead(string targetName, uint type, int reservedFlag, out IntPtr credentialPtr);

    [DllImport("advapi32.dll", EntryPoint = "CredFree", SetLastError = true)]
    private static extern void CredFree([In] IntPtr credentialPtr);

    [DllImport("advapi32.dll", EntryPoint = "CredDeleteW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CredDelete(string targetName, uint type, int reservedFlags);

    /// <summary>
    /// Saves a secret to Windows Credential Manager under target "ModernSapiAdapter:{providerName}:{key}".
    /// </summary>
    public static bool SaveUserCredential(string providerName, string key, string secret)
    {
        string target = $"ModernSapiAdapter:{providerName}:{key}";
        byte[] secretBytes = Encoding.UTF8.GetBytes(secret);
        IntPtr blobPtr = Marshal.AllocHGlobal(secretBytes.Length);

        try
        {
            Marshal.Copy(secretBytes, 0, blobPtr, secretBytes.Length);

            CREDENTIAL cred = new CREDENTIAL
            {
                Type = CRED_TYPE_GENERIC,
                TargetName = target,
                CredentialBlobSize = secretBytes.Length,
                CredentialBlob = blobPtr,
                Persist = CRED_PERSIST_LOCAL_MACHINE,
                UserName = Environment.UserName,
                Comment = "ModernSapiAdapter Secure Credential"
            };

            return CredWrite(ref cred, 0);
        }
        finally
        {
            Marshal.FreeHGlobal(blobPtr);
        }
    }

    /// <summary>
    /// Reads a secret from Windows Credential Manager. Returns null if missing or failed.
    /// </summary>
    public static string? ReadUserCredential(string providerName, string key)
    {
        string target = $"ModernSapiAdapter:{providerName}:{key}";
        if (CredRead(target, CRED_TYPE_GENERIC, 0, out IntPtr credPtr))
        {
            try
            {
                CREDENTIAL cred = Marshal.PtrToStructure<CREDENTIAL>(credPtr);
                if (cred.CredentialBlob != IntPtr.Zero && cred.CredentialBlobSize > 0)
                {
                    byte[] secretBytes = new byte[cred.CredentialBlobSize];
                    Marshal.Copy(cred.CredentialBlob, secretBytes, 0, cred.CredentialBlobSize);
                    return Encoding.UTF8.GetString(secretBytes);
                }
            }
            finally
            {
                CredFree(credPtr);
            }
        }
        return null;
    }

    /// <summary>
    /// Deletes a secret from Windows Credential Manager.
    /// </summary>
    public static bool DeleteUserCredential(string providerName, string key)
    {
        string target = $"ModernSapiAdapter:{providerName}:{key}";
        return CredDelete(target, CRED_TYPE_GENERIC, 0);
    }

    /// <summary>
    /// Encrypts plain text using Machine-Wide DPAPI (DataProtectionScope.LocalMachine) into Base64 blob.
    /// </summary>
    public static string EncryptMachineDpapi(string plainText)
    {
        if (string.IsNullOrEmpty(plainText)) return string.Empty;
        byte[] plainBytes = Encoding.UTF8.GetBytes(plainText);
        byte[] cipherBytes = ProtectedData.Protect(plainBytes, null, DataProtectionScope.LocalMachine);
        return Convert.ToBase64String(cipherBytes);
    }

    /// <summary>
    /// Decrypts Base64 DPAPI blob using Machine-Wide DPAPI scope. Returns null on failure.
    /// </summary>
    public static string? DecryptMachineDpapi(string base64Cipher)
    {
        if (string.IsNullOrEmpty(base64Cipher)) return null;
        try
        {
            byte[] cipherBytes = Convert.FromBase64String(base64Cipher);
            byte[] plainBytes = ProtectedData.Unprotect(cipherBytes, null, DataProtectionScope.LocalMachine);
            return Encoding.UTF8.GetString(plainBytes);
        }
        catch
        {
            return null;
        }
    }
}
