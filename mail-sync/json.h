/* json.h - a minimal JSON parser, just enough for busctl --json output.
 * Values are a tree; objects keep insertion order. No streaming. */
#ifndef WMAIL_JSON_H
#define WMAIL_JSON_H

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct Json Json;
struct Json {
	JType t;
	int b;		/* J_BOOL */
	double num;	/* J_NUM */
	char *s;	/* J_STR */
	Json **items;	/* J_ARR, or J_OBJ values */
	char **keys;	/* J_OBJ only */
	int n, cap;
};

Json *json_parse(const char *text, char **err);
void json_free(Json *j);

/* Object lookup, NULL if missing or not an object. */
Json *json_get(Json *j, const char *key);

/* Unwrap busctl variant wrappers {"type":..., "data":...} repeatedly. */
Json *json_unwrap(Json *j);

/* Convenience: unwrapped string/bool value, or NULL. */
const char *json_as_str(Json *j);
int json_as_bool(Json *j, int dflt);

#endif
