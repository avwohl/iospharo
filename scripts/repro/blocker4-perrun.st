"Run testAddTag 6 times, report pass/fail + exact error class+msg each run.
No allocation churn — isolates test-execution-state accumulation."
| err res |
err := Stdio stderr.
err nextPutAll: 'PERRUN start'; lf; flush.
1 to: 6 do: [:run |
  res := [(PackageOnModelTest selector: #testAddTag) runCase. 'PASS']
    on: Error do: [:e | 'FAIL ', e class name, ' : ', (e messageText ifNil: ['<nil>'])].
  err nextPutAll: 'PERRUN run='; print: run; nextPutAll: ' => '; nextPutAll: res; lf; flush].
err nextPutAll: 'PERRUN done'; lf; flush.
