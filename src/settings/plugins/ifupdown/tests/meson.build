test_unit = 'test-ifupdown'

exe = executable(
  test_unit,
  test_unit + '.c',
  dependencies: test_nm_dep,
  c_args: nm_build_cflags,
  link_with: libnms_ifupdown_core
)

test(
  'ifupdown/' + test_unit,
  test_script,
  args: test_args + [exe.full_path()]
)
