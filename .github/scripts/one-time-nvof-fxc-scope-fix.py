from pathlib import Path

path = Path('Source/NvidiaOpticalFlowDenseSeed.hlsl')
text = path.read_text(encoding='utf-8')
old_init = '''    for (uint bin = 0u; bin < CutHistogramBins; ++bin) {
        histA[bin] = 0u;
        histB[bin] = 0u;
    }
'''
new_init = '''    for (uint initBin = 0u; initBin < CutHistogramBins; ++initBin) {
        histA[initBin] = 0u;
        histB[initBin] = 0u;
    }
'''
old_intersect = '''    for (uint bin = 0u; bin < CutHistogramBins; ++bin) {
        intersectionCount += min(histA[bin], histB[bin]);
    }
'''
new_intersect = '''    for (uint histBin = 0u; histBin < CutHistogramBins; ++histBin) {
        intersectionCount += min(histA[histBin], histB[histBin]);
    }
'''
if old_init not in text or old_intersect not in text:
    raise RuntimeError('Expected histogram loops were not found')
text = text.replace(old_init, new_init, 1).replace(old_intersect, new_intersect, 1)
path.write_text(text, encoding='utf-8', newline='\n')
print('Applied fxc loop-scope fix.')
