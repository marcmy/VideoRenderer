
    std::wcout << std::fixed << std::setprecision(2);
    PrintErrorSummary(L"v6 occlusion-aware boundary", v6Boundary);
    PrintErrorSummary(L"v7 edge-search boundary", searchBoundary);
    PrintErrorSummary(L"v7 refined stable background", refinedBackground);
    PrintErrorSummary(L"v7 refined moving object interior", refinedObject);
    PrintErrorSummary(L"v7 refined boundary", refinedBoundary);
    std::wcout << L"Edge-search MAE improvement over v6: "
               << searchMaeImprovement << L"%\n"
               << L"Final boundary improvement over v6: MAE="
               << refinedMaeImprovement << L"%, P95="
               << refinedP95Improvement << L"%\n";

    const std::filesystem::path directory = std::filesystem::current_path();
    const auto refinedBgra = OcclusionSwapRedBlue(refinedRgba);
    const auto selectionBgra = OcclusionSwapRedBlue(selectionRgba);
    const auto confidenceBgra = OcclusionSwapRedBlue(confidenceRgba);
    const auto repairBgra = OcclusionSwapRedBlue(repairRgba);
    const auto v6Difference = MakeDifferenceImage(v6Rgba, expectedRgba);
    const auto refinedDifference = MakeDifferenceImage(refinedRgba, expectedRgba);

    const std::filesystem::path refinedPath =
        directory / L"NativeNvofBoundaryRefined.bmp";
    const std::filesystem::path differencePath =
        directory / L"NativeNvofBoundaryRefinedDiff.bmp";
    const std::filesystem::path v6DifferencePath =
        directory / L"NativeNvofBoundaryV6Diff.bmp";
    const std::filesystem::path selectionPath =
        directory / L"NativeNvofBoundarySelection.bmp";
    const std::filesystem::path confidencePath =
        directory / L"NativeNvofBoundaryConfidence.bmp";
    const std::filesystem::path repairPath =
        directory / L"NativeNvofBoundaryRepair.bmp";

    const bool saved =
        SaveBitmap(refinedPath, refinedBgra) &&
        SaveBitmap(differencePath, refinedDifference) &&
        SaveBitmap(v6DifferencePath, v6Difference) &&
        SaveBitmap(selectionPath, selectionBgra) &&
        SaveBitmap(confidencePath, confidenceBgra) &&
        SaveBitmap(repairPath, repairBgra);
    if (saved) {
        std::wcout << L"Refined midpoint: " << refinedPath.wstring() << L'\n'
                   << L"Refined amplified difference: " << differencePath.wstring() << L'\n'
                   << L"v6 amplified difference: " << v6DifferencePath.wstring() << L'\n'
                   << L"Edge-search selection: " << selectionPath.wstring() << L'\n'
                   << L"Edge-search confidence: " << confidencePath.wstring() << L'\n'
                   << L"Repair mask: " << repairPath.wstring() << L'\n';
    }

    const bool stableRegionsPassed =
        refinedBackground.meanAbsoluteError <= 1.0 &&
        refinedBackground.percentile95 <= 3 &&
        refinedObject.meanAbsoluteError <= 2.0 &&
        refinedObject.percentile95 <= 8;
    const bool boundaryImproved =
        refinedBoundary.meanAbsoluteError <
            v6Boundary.meanAbsoluteError * 0.98 &&
        refinedBoundary.percentile95 <= v6Boundary.percentile95;
    const bool passed = stableRegionsPassed && boundaryImproved;

    std::wcout << L"BOUNDARY REFINEMENT RESULT: "
               << (passed ? L"PASS" : L"FAIL") << L'\n';
    return passed ? 0 : 21;
}
