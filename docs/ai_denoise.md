# AI Denoise

AI Denoise removes the sensor noise of a RAW photo with a neural network. The network reads the
RAW mosaic of the photo and gives a full-color image, so it does the demosaic of the photo as
well. Alcedo Studio runs it in the Develop stage of the full-size render: in the editor and in
the export. The RAW file stays the photo; the denoised image lives in the render cache of the
running application.

AI Denoise works best on photos with visible noise: high ISO, dark scenes, and shadows that
were raised a lot. A clean photo at a low ISO looks the same with it and renders more slowly.

The product plan is the
[AI Denoise Integration Plan](roadmap/alcedo_studio/raw-processing/ai_denoise/ai_denoise_integration_plan.md).
The parameters are in the table at the end.

## Requirements

| Platform | Device that runs the model |
| --- | --- |
| Windows | A GPU with Direct3D 12 (DirectML), with the CUDA and the OpenCL render backend |
| macOS on Apple silicon | The Neural Engine (Core ML) |

AI Denoise reads Bayer RAW files: the sensors of most cameras. The model is a download of the
AI Models page: **Settings > AI Models > AI Denoise**. It is stored in the AI model folder of
the same page.

## Turn on AI Denoise

1. Open the photo in the editor.
2. In the **RAW Decode** panel, turn on **AI Denoise**. The switch is above the demosaic method.
3. The photo renders again with the model. The pill at the bottom left of the viewport shows the
   step of the render (`Reading RAW`, `AI Denoise` with the tile count, `Developing`), and the bar
   at the bottom edge shows its progress.

While AI Denoise is on, the demosaic method shows one locked segment, **AI Denoise**. Turn the
switch off to return to the method that the photo had.

The switch is one entry of the edit history: **Undo** turns it off again. The photo records the
version of the model that was installed when you turned it on. A project opened on another
computer runs the model of that computer.

Before the model is downloaded, the switch opens the AI Models page. Download the model there
and turn the switch on again.

For an X-Trans or a monochrome RAW file the switch is disabled, and its tooltip names the
reason.

## Several photos

Copy and Paste of the adjustments carry the switch:

1. Turn on AI Denoise in one photo.
2. **Copy Adjustments**, and select the **RAW Decode** item.
3. Select the other photos in the Library and **Paste Adjustments**.

The **RAW Decode** item holds the switch, the demosaic method, and the highlight reconstruction.
The paste sets the switch of each photo; each photo runs the model when it renders at full size.
Paste the RAW Decode item of a photo with the switch off to turn it off in several photos.

## Thumbnails and the export

The Library thumbnails render from a reduced RAW mosaic and show the photo with the standard
demosaic. The editor and the export render at full size and show the denoised photo.

The export runs the model for each photo whose switch is on. The first photo of a size takes
longer, because the model session starts.

## Errors

When the model cannot run, the editor shows a card on the viewport, and the export lists the
photo with the AI Denoise status. Each error has the control **Turn Off AI Denoise**: it turns
the switch off (one history entry) and renders the photo again, or puts the export item back in
the queue.

| Card | Cause | What to do |
| --- | --- | --- |
| AI Denoise model not installed | The model folder lacks the AI Denoise model | **Open AI Models** and download it, or **Turn Off AI Denoise** |
| This GPU is too slow for AI Denoise | The projected time of the photo is above the slow-device limit | **Turn Off AI Denoise**; the device stays marked as slow until Alcedo Studio restarts |
| AI Denoise does not support this sensor | A pasted switch reached an X-Trans or monochrome RAW file, or a raster image | **Turn Off AI Denoise** |
| AI Denoise failed | The runtime reported an error, for example of the GPU or of its memory | Read the text of the card; **Turn Off AI Denoise** renders the photo with the standard demosaic |

## Automation

The `alcedo-cli` commands `denoise.status` and `denoise.set` read and write the switch of the
photo that the editor has open; `adjustments.copy` and `adjustments.paste` of the `raw_decode`
item carry it to other photos. The
[alcedo-skills command reference](https://github.com/zidage/alcedo-skills/blob/main/skills/alcedo/references/reference/denoise.md)
describes them. The alcedo agent skill turns AI Denoise on when you ask for noise reduction,
and by itself only for a high ISO photo whose noise is clearly visible.

## Parameters

| Parameter | Meaning | Value | Source |
| --- | --- | --- | --- |
| Model | UtNet2 Bayer network of [alcedo-denoise-lab](https://github.com/zidage/alcedo-denoise-lab), FP16 ONNX graph | Package `alcedo-ai-denoise` v1 | Plan D1, D2, DN12 |
| Model version | SHA-256 of the graph file, recorded by the switch | 64 hexadecimal digits | Plan D10 |
| Target time | Measured model time of a 45.4 MP photo | 0.72 s (RTX 3080 Laptop GPU), 0.60 s (MacBook Air M4) | Plan U1 |
| Slow-device limit | Largest projected model time for each megapixel of the photo | 0.05 s (2.3 s at 45.4 MP) | Plan section 9 |
| Tile size | Side of one model tile in packed pixels, the largest that fits the free memory | 768, 1024, or 1536 | Plan section 9 |
| Tile overlap | Packed pixels that two tiles share | 96 | Plan DN1 |
