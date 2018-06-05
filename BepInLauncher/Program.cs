using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Windows.Forms;
using SimpleJSON;

namespace BepInLauncher
{
    class Program
    {
        public static string GamePath { get; set; }

        [STAThread]
        static void Main(string[] args)
        {
            OpenFileDialog dialog = new OpenFileDialog
            {
                    CheckFileExists = true,
                    Multiselect = false,
                    AddExtension = true,
                    Filter = "Unity Game Executable (*.exe)|*.exe"
            };

            DialogResult res = dialog.ShowDialog();

            if (res == DialogResult.OK)
            {
                GamePath = dialog.FileName;
                Console.WriteLine($"Selected file: {GamePath}");

                Console.WriteLine("Creating file system tree");
                CreateFileSystemTree();

                Console.WriteLine("Launching the game with custom Doorstop args!");
                LaunchGame();
            }
        }

        static void LaunchGame()
        {
            Process p = Process.Start(GamePath, $"--doorstop-enable true --doorstop-target \"{Path.GetFullPath("BepInEx\\bin\\BepInPreloader.dll")}\"");
        }

        static void CreateFileSystemTree()
        {
            JSONObject o = new JSONObject();

            o["BepInEx"] = new JSONObject();

            GenTree(o["BepInEx"].AsObject, "BepInEx");

            if (!Directory.Exists("__temp__"))
                Directory.CreateDirectory("__temp__");
            GenTree(o["BepInEx"].AsObject, "__temp__");

            foreach (string modDir in Directory.GetDirectories("mods"))
                GenTree(o, modDir);

            StringBuilder sb = new StringBuilder();

            o.WriteToStringBuilder(sb, 4, 1, JSONTextMode.Compact);

            File.WriteAllText("vfs.json", sb.ToString());
        }

        static void GenTree(JSONObject treeRoot, string path)
        {
            path = Path.GetFullPath(path);

            // TODO: Handle ambiguities

            foreach (string directory in Directory.GetDirectories(path))
            {
                string dirName = Path.GetFileName(directory);

                JSONObject subdir = treeRoot[dirName].AsObject;
                GenTree(subdir, directory);
            }

            foreach (string file in Directory.GetFiles(path))
            {
                string fileName = Path.GetFileName(file);

                treeRoot[fileName] = file;
            }
        }
    }
}
