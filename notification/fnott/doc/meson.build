scdoc_prog = find_program(scdoc.get_variable('scdoc'), native: true)

if scdoc.found()
  foreach man_src : [{'name': 'fnott', 'section': 1},
                     {'name': 'fnott.ini', 'section': 5},
                     {'name': 'fnottctl', 'section': 1}]
    name = man_src['name']
    section = man_src['section']
    out = '@0@.@1@'.format(name, section)

    custom_target(
      out,
      output: out,
      input: '@0@.@1@.scd'.format(name, section),
      command: scdoc_prog.full_path(),
      capture: true,
      feed: true,
      install: true,
      install_dir: join_paths(get_option('mandir'), 'man@0@'.format(section)))
  endforeach
endif
