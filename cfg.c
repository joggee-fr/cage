#include <scfg.h>
#include <string.h>

#include "cfg.h"

static bool parse_multi_output(const struct scfg_directive *dir, struct cg_server *server) {
	if (dir->params_len != 1) {
		fprintf(stderr, "directive 'multi-output': expected at least one param\n");
		fprintf(stderr, "(on line %d)\n", dir->lineno);
		return NULL;
	}

	if (dir->params_len > 0) {
			if (strcmp(dir->params[0], "last") == 0) {
				server->output_mode = CAGE_MULTI_OUTPUT_MODE_LAST;
			} else if (strcmp(dir->params[0], "extend") == 0) {
				server->output_mode = CAGE_MULTI_OUTPUT_MODE_EXTEND;
			}
	}

	// TODO
	return true;
}

bool parse_config_file(const char *path, struct cg_server *server) {
	struct scfg_block block = {0};
	if (scfg_load_file(&block, path) != 0) {
		fprintf(stderr, "failed to parse config file\n");
		return false;
	}

	for (size_t i = 0; i < block.directives_len; i++) {
		struct scfg_directive *dir = &block.directives[i];

		fprintf(stderr, "%s %s\n", dir->name, dir->params[0]);

		if (strcmp(dir->name, "multi-output")) {
			parse_multi_output(dir, server);
		}

	}

	scfg_block_finish(&block);
	return true;
}