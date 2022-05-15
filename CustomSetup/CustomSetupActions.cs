using System;
using System.Collections;
using System.Collections.Generic;
using System.ComponentModel;
using System.Configuration;
using System.IO;

// Reference: https://www.c-sharpcorner.com/article/how-to-perform-custom-actions-and-upgrade-using-visual-studio-installer/
namespace SetupCustomActions
{
    [RunInstaller(true)]
    public partial class CustomActions : System.Configuration.Install.Installer
    {
        public CustomActions()
        {
        }

        protected override void OnAfterInstall(IDictionary savedState)
        {
            var installPath = Path.GetDirectoryName(base.Context.Parameters["AssemblyPath"]);
            var jsonName = "XR_APILAYER_NOVENDOR_d3d12on11_interop.json";
            var jsonPath = installPath + "\\" + jsonName;

            // We want to add our layer at the very end, so that any other layers like the OpenXR Toolkit layer is before us, and sees proper D3D12 support.

            Microsoft.Win32.RegistryKey key;
            key = Microsoft.Win32.Registry.LocalMachine.CreateSubKey("SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit");

            // Delete other occurrences of our layer.
            var existingValues = key.GetValueNames();
            foreach (var value in existingValues)
            {
                if (value.EndsWith("\\" + jsonName))
                {
                    key.DeleteValue(value);
                }
            }

            // Insert ourselves.
            key.SetValue(jsonPath, 0);

            key.Close();

            base.OnAfterInstall(savedState);
        }
    }
}
