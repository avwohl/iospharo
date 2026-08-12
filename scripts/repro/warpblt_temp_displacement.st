| log bad n done procs |
log := Stdio stdout.
bad := OrderedCollection new.
n := 0. done := 0.
procs := (1 to: 6) collect: [:k |
  [ | src dst c |
    c := (k \\ 3) + 1.
    #(8 16 32 1) do: [:d |
      [ src := Form extent: 17@13 depth: d.
        dst := Form extent: 40@31 depth: d.
        1 to: 60 do: [:i |
          [(WarpBlt toForm: dst)
              sourceForm: src destRect: (dst boundingBox);
              combinationRule: 3;
              cellSize: c;
              warpBits] ensure: [n := n + 1].
          (i \\ 7 = 0) ifTrue: [ thisContext sender ifNotNil: [:s | s tempNames ] ].
          Processor yield] ]
        on: Error do: [:ex |
          bad add: k printString,':',d printString,'/',c printString,' -> ',
                ex class name, ': ', (ex messageText ifNil: ['']) ] ].
    done := done + 1 ] forkAt: Processor userBackgroundPriority ].
[ done < 6 ] whileTrue: [ (Delay forMilliseconds: 20) wait ].
log nextPutAll: 'WARP2 calls=', n printString, ' errors=', bad size printString; lf.
bad do: [:s | log nextPutAll: 'WARP2ERR ', s; lf].
log nextPutAll: 'WARP2-DONE'; lf; flush.
