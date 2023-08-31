wl_protocol_dir = wayland_protos.get_variable('pkgdatadir')

wayland_scanner = find_program('wayland-scanner', native: true)
wayland_scanner_server = generator(
  wayland_scanner,
  output: '@BASENAME@-protocol.h',
  arguments: ['server-header', '@INPUT@', '@OUTPUT@'],
)

server_protocols = [
	[wl_protocol_dir, 'stable/xdg-shell/xdg-shell.xml'],
	['wlr-output-power-management-unstable-v1.xml'],
]

server_protos_headers = []

foreach p : server_protocols
  xml = join_paths(p)
  server_protos_headers += wayland_scanner_server.process(xml)
endforeach

server_protos = declare_dependency(
  sources: server_protos_headers,
)