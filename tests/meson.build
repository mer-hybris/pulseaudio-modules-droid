if gbinder_dep.found()
  fmq_test = executable('aidl-fmq-test',
    ['aidl-fmq-test.c', '../src/common/aidl-fmq.c'],
    c_args : pa_c_args,
    dependencies : pulsecore_dep,
    include_directories : [configinc, include_directories('../src/common/include')],
  )
  test('aidl-fmq', fmq_test)
endif
