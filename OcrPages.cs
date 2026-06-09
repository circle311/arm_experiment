using System;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Windows.Globalization;
using Windows.Graphics.Imaging;
using Windows.Media.Ocr;
using Windows.Storage;

class OcrPages
{
    static async Task<string> OcrImageAsync(string path, OcrEngine engine)
    {
        var file = await StorageFile.GetFileFromPathAsync(Path.GetFullPath(path));
        using (var stream = await file.OpenAsync(FileAccessMode.Read))
        {
            var decoder = await BitmapDecoder.CreateAsync(stream);
            var bitmap = await decoder.GetSoftwareBitmapAsync();
            var result = await engine.RecognizeAsync(bitmap);
            return string.Join(Environment.NewLine, result.Lines.Select(line =>
                string.Join(" ", line.Words.Select(word => word.Text))));
        }
    }

    static void Main(string[] args)
    {
        var inputDir = args.Length > 0 ? args[0] : "_pdf_pages";
        var outputDir = args.Length > 1 ? args[1] : "_ocr_text";
        Directory.CreateDirectory(outputDir);

        var engine = OcrEngine.TryCreateFromLanguage(new Language("zh-Hans-CN"));
        if (engine == null)
        {
            throw new InvalidOperationException("Cannot create zh-Hans-CN OCR engine.");
        }

        foreach (var image in Directory.GetFiles(inputDir, "*.png").OrderBy(x => x))
        {
            var text = OcrImageAsync(image, engine).GetAwaiter().GetResult();
            var name = Path.GetFileNameWithoutExtension(image) + ".txt";
            File.WriteAllText(Path.Combine(outputDir, name), text);
            Console.WriteLine(Path.GetFileName(image) + ": " + text.Length);
        }
    }
}
