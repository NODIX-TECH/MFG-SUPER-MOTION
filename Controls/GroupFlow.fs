namespace ARMO.Controls

open System
open Avalonia
open Avalonia.Controls

/// Lays groups of settings out side by side where the page is wide enough for two columns - each group under
/// the shorter column so far - and under each other where it is not. Sizes in logical pixels.
type GroupFlow() =
    inherit Panel()
    let gap = 12.
    let columnsFor (width: float) = if Double.IsFinite width && width >= 760. then 2 else 1

    override this.MeasureOverride(available: Size) =
        let columns = columnsFor available.Width
        let width = if Double.IsFinite available.Width then (available.Width - gap * float (columns - 1)) / float columns else 420.
        let heights = Array.zeroCreate<float> columns
        for child in this.Children do
            if child.IsVisible then
                child.Measure(Size(width, Double.PositiveInfinity))
                let column = if columns = 2 && heights[1] < heights[0] then 1 else 0
                heights[column] <- heights[column] + (if heights[column] > 0. then gap else 0.) + child.DesiredSize.Height
        Size((if Double.IsFinite available.Width then available.Width else width), Array.max heights)

    override this.ArrangeOverride(final: Size) =
        let columns = columnsFor final.Width
        let width = (final.Width - gap * float (columns - 1)) / float columns
        let heights = Array.zeroCreate<float> columns
        for child in this.Children do
            if child.IsVisible then
                let column = if columns = 2 && heights[1] < heights[0] then 1 else 0
                let top = heights[column] + (if heights[column] > 0. then gap else 0.)
                child.Arrange(Rect(float column * (width + gap), top, width, child.DesiredSize.Height))
                heights[column] <- top + child.DesiredSize.Height
        final


/// 1.1.0, the main page's two cards (frame generation, upscaling): side by side where the page is wide enough - the
/// first taking `Share` of the width - and under each other where it is not. Both as tall as the taller one.
type CardPair() =
    inherit Panel()
    let gap = 12.
    member val Share = 0.58 with get, set
    member val Wide = 820. with get, set

    member private this.Visible() = this.Children |> Seq.filter (fun c -> c.IsVisible) |> Seq.toArray

    override this.MeasureOverride(available: Size) =
        let shown = this.Visible()
        let side = shown.Length = 2 && Double.IsFinite available.Width && available.Width >= this.Wide
        if side then
            let first = (available.Width - gap) * this.Share
            shown[0].Measure(Size(first, Double.PositiveInfinity))
            shown[1].Measure(Size(available.Width - gap - first, Double.PositiveInfinity))
            Size(available.Width, max shown[0].DesiredSize.Height shown[1].DesiredSize.Height)
        else
            let width = if Double.IsFinite available.Width then available.Width else 600.
            let mutable height = 0.
            for child in shown do
                child.Measure(Size(width, Double.PositiveInfinity))
                height <- height + (if height > 0. then gap else 0.) + child.DesiredSize.Height
            Size(width, height)

    override this.ArrangeOverride(final: Size) =
        let shown = this.Visible()
        if shown.Length = 2 && final.Width >= this.Wide then
            let first = (final.Width - gap) * this.Share
            let height = max shown[0].DesiredSize.Height shown[1].DesiredSize.Height
            shown[0].Arrange(Rect(0., 0., first, height))
            shown[1].Arrange(Rect(first + gap, 0., final.Width - gap - first, height))
        else
            let mutable top = 0.
            for child in shown do
                child.Arrange(Rect(0., top, final.Width, child.DesiredSize.Height))
                top <- top + child.DesiredSize.Height + gap
        final
