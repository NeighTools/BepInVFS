using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using SimpleJSON;

namespace BepInLauncher
{
    class Program
    {
        public static string GamePath { get; set; }

        static void Main(string[] args)
        {
            GamePath = args[0];

            Console.WriteLine("Creating file system tree");
            CreateFileSystemTree();
            Console.WriteLine("Done!");
            Console.ReadKey();
            Console.WriteLine("Launching the game!");
            LaunchGame();
            Console.WriteLine("Done!");
            Console.ReadKey();
        }

        static void LaunchGame()
        {
            Process p = Process.Start(GamePath, "--doorstop-enable true --doorstop-target \"BepInEx\\bin\\BepInPreloader.dll\"");
        }

        static void CreateFileSystemTree()
        {
            JSONObject o = new JSONObject();

            o["BepInEx"] = new JSONObject();

            GenTree(o["BepInEx"].AsObject, "BepInEx");

            foreach (string modDir in Directory.GetDirectories("mods"))
                GenTree(o, modDir);

            StringBuilder sb = new StringBuilder();

            o.WriteToStringBuilder(sb, 4, 1, JSONTextMode.Compact);

            File.WriteAllText("vfs.json", sb.ToString());
        }

        static void GenTree(JSONObject treeRoot, string path)
        {
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
