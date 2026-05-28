"Render canonical Roassal3 visualizations to PNG to validate the rendering
pipeline end-to-end.  Each render writes /tmp/render_<tag>.png and a one-line
status to /tmp/render_status.txt.

Goal: produce a fixed set of PNG baselines that, going forward, we can diff
against stock-Cog renders to spot pixel-level regressions."

Object subclass: #RenderHarness
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Runner'!

RenderHarness class compile: 'startUp: resuming
  | status |
  resuming ifFalse: [^ self].
  status := ''/tmp/render_status.txt'' asFileReference writeStream.
  [
    self render: ''empty_canvas'' status: status block: [
      | c |
      c := RSCanvas new.
      c extent: 200 @ 150.
      c].
    self render: ''single_box''   status: status block: [
      | c b |
      c := RSCanvas new.
      c extent: 200 @ 150.
      b := RSBox new size: 80; color: Color red.
      c add: b.
      c].
    self render: ''bar_plot'' status: status block: [
      RSBarPlot new
        y: #(3 1 4 1 5 9 2 6 5 3);
        canvas].
    self render: ''line_plot'' status: status block: [
      RSLinePlot new
        y: #(0 1 4 9 16 25 36 49 64 81);
        canvas].
  ] ensure: [ status close ].
  Smalltalk exitSuccess' classified: 'startup'!

RenderHarness class compile: 'render: tag status: statusStream block: aBlock
  | canvas form path |
  [
    canvas := aBlock value.
    form := canvas asForm.
    path := ''/tmp/render_'', tag, ''.png''.
    PNGReadWriter putForm: form onFileNamed: path.
    statusStream
      nextPutAll: tag; tab;
      print: form width; nextPutAll: ''x''; print: form height; tab;
      nextPutAll: ''OK''; lf.
  ] on: Error do: [:e |
    statusStream
      nextPutAll: tag; tab;
      nextPutAll: ''ERR ''; nextPutAll: e class name; nextPutAll: '': '';
      nextPutAll: (e messageText ifNil: [''?'']); lf]' classified: 'render'!

RenderHarness class compile: 'shutDown: quitting' classified: 'startup'!

SessionManager default
  register: (ClassSessionHandler forClassNamed: #RenderHarness)
  inCategory: SessionManager default userCategory atPriority: 80!
