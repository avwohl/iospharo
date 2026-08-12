"Name the displacement.  On each error, walk the signaler's sender chain to the
 frame whose receiver is a WarpBlt and print every temp paired with its class,
 plus the frame's pc and stackp.  WarpBlt>>warpBitsSmoothing:sourceMap: temps
 are, in order:
   n sourceMap deltaP12 deltaP43 pA pB deltaPAB sp fixedPtOne picker poker pix nSteps
 so a shifted slot shows up as a value sitting one or more positions from where
 its name says it belongs."

| log bad n done procs describe |
log := Stdio stdout.
bad := OrderedCollection new.
n := 0. done := 0.

describe := [:ex |
    | ctx names out |
    ctx := ex signalerContext.
    [ ctx notNil and: [ (ctx receiver isKindOf: WarpBlt) not ] ]
        whileTrue: [ ctx := ctx sender ].
    ctx
        ifNil: [ 'no WarpBlt frame found' ]
        ifNotNil: [
            names := [ ctx tempNames ] on: Error do: [:e | #() ].
            out := String streamContents: [:s |
                s << ctx method selector << ' pc=' << ctx pc printString
                  << ' stackp=' << ([ ctx stackPtr printString ] on: Error do: [:e | '?' ]) << ' | '.
                names doWithIndex: [:nm :i |
                    | v |
                    v := [ ctx tempAt: i ] on: Error do: [:e | #err ].
                    s << nm << '=' << ([ v class name ] on: Error do: [:e | '?' ]) << ' ' ] ].
            out ] ].

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
          bad size < 6 ifTrue: [
            bad add: 'd=', d printString, ' c=', c printString, ' ',
                     ex class name, ': ', (ex messageText ifNil: ['']), ' ||| ',
                     (describe value: ex) ] ] ].
    done := done + 1 ] forkAt: Processor userBackgroundPriority ].
[ done < 6 ] whileTrue: [ (Delay forMilliseconds: 20) wait ].
log nextPutAll: 'LOC calls=', n printString, ' errors=', bad size printString; lf.
bad do: [:s | log nextPutAll: 'LOCERR ', s; lf].
log nextPutAll: 'LOC-DONE'; lf; flush.
