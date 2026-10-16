"excdiff.st -- scenarios run on our VM and on Cog; diff the two answers.
Run: test_load_image P.image eval "$(cat excdiff.st)"; for Cog, arch -x86_64 Pharo --headless C.image eval ...
See docs/vm-compat-bugs.md, 2026-10-17."

| results run |
results := OrderedCollection new.
run := [:name :blk | | l r | l := OrderedCollection new. r := [ blk value: l ] on: Error do: [:e | 'ERR ', e class name ]. results add: name -> { r. l asArray } ].
Object compile: 'exNlr: l  #(1 2) do: [:i | [ [ ^ i ] ensure: [ l add: #in ] ] ensure: [ l add: #out ] ]. ^ 0'.
Object compile: 'exNlr2: l  [ #(1 2) do: [:i | [ i = 2 ifTrue: [ ^ i ] ] ifCurtailed: [ l add: i ] ] ] ensure: [ l add: #e ]. ^ 0'.
Object compile: 'exDeep: n l: l  n = 0 ifTrue: [ ^ Error signal: #deep ]. ^ [ self exDeep: n - 1 l: l ] ensure: [ l add: n ]'.
run value: #nlrEnsures value: [:l | nil exNlr: l ].
run value: #nlrCurtailed value: [:l | nil exNlr2: l ].
run value: #deepEnsure value: [:l | [ nil exDeep: 5 l: l ] on: Error do: [:e | l add: #handler. e return: #r ] ].
run value: #deepEnsureRetry value: [:l | | n | n := 0. [ n := n + 1. n < 3 ifTrue: [ nil exDeep: 2 l: l ]. n ] on: Error do: [:e | l add: #h. e retry ] ].
run value: #signalInEnsure value: [:l | [ [ l add: 1 ] ensure: [ Warning signal: #w. l add: 2 ] ] on: Warning do: [:e | l add: #warn. e resume: nil ] ].
run value: #errorInEnsureDuringUnwind value: [:l | [ [ Error signal: #first ] ensure: [ l add: #ens. [ Error signal: #second ] on: Error do: [:e | l add: e messageText ] ] ] on: Error do: [:e | l add: e messageText. #done ] ].
run value: #resignal value: [:l | [ [ ZeroDivide new signal ] on: ZeroDivide do: [:e | e resignalAs: (Warning new messageText: #rs) ] ] on: Warning do: [:e | l add: e messageText. e return: 9 ] ].
run value: #outerResume value: [:l | [ [ Warning signal: #a ] on: Warning do: [:e | l add: #inner. l add: e outer. #innerDone ] ] on: Warning do: [:e | l add: #outer. e resume: 7 ] ].
run value: #passToOuter value: [:l | [ [ Error signal: #p ] on: Error do: [:e | l add: #i. e pass ] ] on: Error do: [:e | l add: #o. e return: 3 ] ].
run value: #retryUsing value: [:l | [ Error signal ] on: Error do: [:e | e retryUsing: [ l add: #alt. 11 ] ] ].
run value: #signalerContext value: [:l | [ Error signal ] on: Error do: [:e | e signalerContext method selector ] ].
run value: #handlerReturnValue value: [:l | [ Error signal. 5 ] on: Error do: [:e | 6 ] ].
run value: #resumeNonResumable value: [:l | [ [ Error signal ] on: Error do: [:e | e resume: 1 ] ] on: IllegalResumeAttempt do: [:e | #illegal ] ].
run value: #exceptionSets value: [:l | [ ZeroDivide new signal ] on: Warning, ZeroDivide do: [:e | e class name ] ].
run value: #ensureOrderNormal value: [:l | [ [ l add: 1 ] ensure: [ l add: 2 ] ] ensure: [ l add: 3 ]. #ok ].
run value: #ifCurtailedNormal value: [:l | [ 5 ] ifCurtailed: [ l add: #no ] ].
run value: #unwindInBlockNlr value: [:l | | b | b := [:x | [ x > 1 ifTrue: [ #big ] ifFalse: [ #small ] ] ensure: [ l add: x ] ]. { b value: 1. b value: 5 } ].
run value: #signalTwice value: [:l | | ex | ex := Error new. [ ex signal ] on: Error do: [:e | l add: 1 ]. [ ex signal ] on: Error do: [:e | l add: 2 ]. #ok ].
run value: #defaultActionWarning value: [:l | (Warning new messageText: 'x'; yourself) defaultAction class name ].
run value: #ensureAfterReturn value: [:l | [ #v ] ensure: [ l add: #e ] ].
run value: #exceptionDescription value: [:l | [ 1/0 ] on: ZeroDivide do: [:e | { e messageText. e description. e class name. e dividend } ] ].
run value: #mnuReturn value: [:l | [ nil foo: 1 bar: 2 ] on: MessageNotUnderstood do: [:e | { e message selector. e message arguments. e receiver. e return: 4 } ] ].
run value: #mnuResume value: [:l | ([ nil fooz ] on: MessageNotUnderstood do: [:e | e resume: 8 ]) + 1 ].
run value: #timeoutProcess value: [:l | | s r | s := Semaphore new. r := [ s waitTimeoutMilliseconds: 5 ] value. r ].
results asArray
