#ifdef MTRACE
#include <mcheck.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <string.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <sys/resource.h>
#include <pthread.h>
#include <vector>
#include <algorithm>
#include <set>
#include <map>
#include <string>
#include "jsonpull/jsonpull.h"
#include "pool.hpp"
#include "projection.hpp"
#include "memfile.hpp"
#include "main.hpp"
#include "mbtiles.hpp"
#include "geojson.hpp"
#include "geometry.hpp"
#include "options.hpp"
#include "serial.hpp"
#include "text.hpp"
#include "read_json.hpp"
#include "mvt.hpp"

int serialize_feature(struct serialization_state *sst, serial_feature &sf, bool want_dist, bool filters, int maxzoom, bool uses_gamma);

static long long parse_geometry1(struct serialization_state *sst, int t, json_object *j, long long *bbox, drawvec &geom, int op, json_object *feature, long long &prev, long long &offset, bool &has_prev) {
	parse_geometry(t, j, geom, op, sst->fname, sst->line, feature);

	for (size_t i = 0; i < geom.size(); i++) {
		if (geom[i].op == VT_MOVETO || geom[i].op == VT_LINETO) {
			long long x = geom[i].x;
			long long y = geom[i].y;

			if (additional[A_DETECT_WRAPAROUND]) {
				x += offset;
				if (has_prev) {
					if (x - prev > (1LL << 31)) {
						offset -= 1LL << 32;
						x -= 1LL << 32;
					} else if (prev - x > (1LL << 31)) {
						offset += 1LL << 32;
						x += 1LL << 32;
					}
				}

				has_prev = true;
				prev = x;
			}

			if (x < bbox[0]) {
				bbox[0] = x;
			}
			if (y < bbox[1]) {
				bbox[1] = y;
			}
			if (x > bbox[2]) {
				bbox[2] = x;
			}
			if (y > bbox[3]) {
				bbox[3] = y;
			}

			if (!*(sst->initialized)) {
				if (x < 0 || x >= (1LL << 32) || y < 0 || y >= (1LL < 32)) {
					*(sst->initial_x) = 1LL << 31;
					*(sst->initial_y) = 1LL << 31;
				} else {
					*(sst->initial_x) = (x >> geometry_scale) << geometry_scale;
					*(sst->initial_y) = (y >> geometry_scale) << geometry_scale;
				}

				*(sst->initialized) = 1;
			}

			geom[i].x = x >> geometry_scale;
			geom[i].y = y >> geometry_scale;
		}
	}

	return geom.size();
}

int serialize_geometry(struct serialization_state *sst, json_object *geometry, json_object *properties, json_object *id, std::set<std::string> *exclude, std::set<std::string> *include, int exclude_all, int basezoom, int layer, double droprate, json_object *tippecanoe, int maxzoom, json_object *feature, std::map<std::string, layermap_entry> *layermap, std::string layername, bool uses_gamma, std::map<std::string, int> const *attribute_types, bool want_dist, bool filters) {
	json_object *geometry_type = json_hash_get(geometry, "type");
	if (geometry_type == NULL) {
		static int warned = 0;
		if (!warned) {
			fprintf(stderr, "%s:%d: null geometry (additional not reported)\n", sst->fname, sst->line);
			json_context(feature);
			warned = 1;
		}

		return 0;
	}

	if (geometry_type->type != JSON_STRING) {
		fprintf(stderr, "%s:%d: geometry type is not a string\n", sst->fname, sst->line);
		json_context(feature);
		return 0;
	}

	json_object *coordinates = json_hash_get(geometry, "coordinates");
	if (coordinates == NULL || coordinates->type != JSON_ARRAY) {
		fprintf(stderr, "%s:%d: feature without coordinates array\n", sst->fname, sst->line);
		json_context(feature);
		return 0;
	}

	int t;
	for (t = 0; t < GEOM_TYPES; t++) {
		if (strcmp(geometry_type->string, geometry_names[t]) == 0) {
			break;
		}
	}
	if (t >= GEOM_TYPES) {
		fprintf(stderr, "%s:%d: Can't handle geometry type %s\n", sst->fname, sst->line, geometry_type->string);
		json_context(feature);
		return 0;
	}

	int tippecanoe_minzoom = -1;
	int tippecanoe_maxzoom = -1;
	std::string tippecanoe_layername;

	if (tippecanoe != NULL) {
		json_object *min = json_hash_get(tippecanoe, "minzoom");
		if (min != NULL && min->type == JSON_NUMBER) {
			tippecanoe_minzoom = min->number;
		}
		if (min != NULL && min->type == JSON_STRING) {
			tippecanoe_minzoom = atoi(min->string);
		}

		json_object *max = json_hash_get(tippecanoe, "maxzoom");
		if (max != NULL && max->type == JSON_NUMBER) {
			tippecanoe_maxzoom = max->number;
		}
		if (max != NULL && max->type == JSON_STRING) {
			tippecanoe_maxzoom = atoi(max->string);
		}

		json_object *ln = json_hash_get(tippecanoe, "layer");
		if (ln != NULL && (ln->type == JSON_STRING || ln->type == JSON_NUMBER)) {
			tippecanoe_layername = std::string(ln->string);
		}
	}

	bool has_id = false;
	unsigned long long id_value = 0;
	if (id != NULL) {
		if (id->type == JSON_NUMBER) {
			if (id->number >= 0) {
				char *err = NULL;
				id_value = strtoull(id->string, &err, 10);

				if (err != NULL && *err != '\0') {
					static bool warned_frac = false;

					if (!warned_frac) {
						fprintf(stderr, "Warning: Can't represent non-integer feature ID %s\n", id->string);
						warned_frac = true;
					}
				} else {
					has_id = true;
				}
			} else {
				static bool warned_neg = false;

				if (!warned_neg) {
					fprintf(stderr, "Warning: Can't represent negative feature ID %s\n", id->string);
					warned_neg = true;
				}
			}
		} else {
			static bool warned_nan = false;

			if (!warned_nan) {
				char *s = json_stringify(id);
				fprintf(stderr, "Warning: Can't represent non-numeric feature ID %s\n", s);
				free(s);  // stringify
				warned_nan = true;
			}
		}
	}

	long long bbox[] = {LLONG_MAX, LLONG_MAX, LLONG_MIN, LLONG_MIN};

	if (!filters) {
		if (tippecanoe_layername.size() != 0) {
			if (layermap->count(tippecanoe_layername) == 0) {
				layermap->insert(std::pair<std::string, layermap_entry>(tippecanoe_layername, layermap_entry(layermap->size())));
			}

			auto ai = layermap->find(tippecanoe_layername);
			if (ai != layermap->end()) {
				layer = ai->second.id;
				layername = tippecanoe_layername;

				if (mb_geometry[t] == VT_POINT) {
					ai->second.points++;
				} else if (mb_geometry[t] == VT_LINE) {
					ai->second.lines++;
				} else if (mb_geometry[t] == VT_POLYGON) {
					ai->second.polygons++;
				}
			} else {
				fprintf(stderr, "Internal error: can't find layer name %s\n", tippecanoe_layername.c_str());
				exit(EXIT_FAILURE);
			}
		} else {
			auto fk = layermap->find(layername);
			if (fk != layermap->end()) {
				if (mb_geometry[t] == VT_POINT) {
					fk->second.points++;
				} else if (mb_geometry[t] == VT_LINE) {
					fk->second.lines++;
				} else if (mb_geometry[t] == VT_POLYGON) {
					fk->second.polygons++;
				}
			}
		}
	}

	size_t nprop = 0;
	if (properties != NULL && properties->type == JSON_HASH) {
		nprop = properties->length;
	}

	char *metakey[nprop];
	std::vector<std::string> metaval;
	metaval.resize(nprop);
	int metatype[nprop];
	size_t m = 0;

	for (size_t i = 0; i < nprop; i++) {
		if (properties->keys[i]->type == JSON_STRING) {
			std::string s(properties->keys[i]->string);

			if (exclude_all) {
				if (include->count(s) == 0) {
					continue;
				}
			} else if (exclude->count(s) != 0) {
				continue;
			}

			int type = -1;
			std::string val;
			stringify_value(properties->values[i], type, val, sst->fname, sst->line, feature, properties->keys[i]->string, attribute_types);

			if (type >= 0) {
				metakey[m] = properties->keys[i]->string;
				metatype[m] = type;
				metaval[m] = val;
				m++;

				type_and_string attrib;
				attrib.type = metatype[m - 1];
				attrib.string = metaval[m - 1];

				if (!filters) {
					auto fk = layermap->find(layername);
					add_to_file_keys(fk->second.file_keys, metakey[m - 1], attrib);
				}
			}
		}
	}

	bool has_prev = false;
	long long prev = 0;
	long long offset = 0;

	drawvec dv;
	parse_geometry1(sst, t, coordinates, bbox, dv, VT_MOVETO, feature, prev, offset, has_prev);
	if (mb_geometry[t] == VT_POLYGON) {
		dv = fix_polygon(dv);
	}

	serial_feature sf;
	sf.layer = layer;
	sf.segment = sst->segment;
	sf.t = mb_geometry[t];
	sf.has_id = has_id;
	sf.id = id_value;
	sf.has_tippecanoe_minzoom = (tippecanoe_minzoom != -1);
	sf.tippecanoe_minzoom = tippecanoe_minzoom;
	sf.has_tippecanoe_maxzoom = (tippecanoe_maxzoom != -1);
	sf.tippecanoe_maxzoom = tippecanoe_maxzoom;
	sf.geometry = dv;
	sf.m = m;
	sf.feature_minzoom = 0;  // Will be filled in during index merging
	sf.seq = *(sst->layer_seq);

	for (size_t i = 0; i < 4; i++) {
		sf.bbox[i] = bbox[i];
	}

	for (size_t i = 0; i < m; i++) {
		sf.full_keys.push_back(metakey[i]);

		serial_val sv;
		sv.type = metatype[i];
		sv.s = metaval[i];

		sf.full_values.push_back(sv);
	}

	return serialize_feature(sst, sf, want_dist, filters, maxzoom, uses_gamma);
}

int serialize_feature(struct serialization_state *sst, serial_feature &sf, bool want_dist, bool filters, int maxzoom, bool uses_gamma) {
	if (want_dist) {
		std::vector<unsigned long long> locs;
		for (size_t i = 0; i < sf.geometry.size(); i++) {
			if (sf.geometry[i].op == VT_MOVETO || sf.geometry[i].op == VT_LINETO) {
				locs.push_back(encode(sf.geometry[i].x << geometry_scale, sf.geometry[i].y << geometry_scale));
			}
		}
		std::sort(locs.begin(), locs.end());
		size_t n = 0;
		double sum = 0;
		for (size_t i = 1; i < locs.size(); i++) {
			if (locs[i - 1] != locs[i]) {
				sum += log(locs[i] - locs[i - 1]);
				n++;
			}
		}
		if (n > 0) {
			double avg = exp(sum / n);
			// Convert approximately from tile units to feet
			double dist_ft = sqrt(avg) / 33;

			*(sst->dist_sum) += log(dist_ft) * n;
			*(sst->dist_count) += n;
		}
		locs.clear();
	}

	bool inline_meta = true;
	// Don't inline metadata for features that will span several tiles at maxzoom
	if (sf.geometry.size() > 0 && (sf.bbox[2] < sf.bbox[0] || sf.bbox[3] < sf.bbox[1])) {
		fprintf(stderr, "Internal error: impossible feature bounding box %llx,%llx,%llx,%llx\n", sf.bbox[0], sf.bbox[1], sf.bbox[2], sf.bbox[3]);
	}
	if (sf.bbox[2] - sf.bbox[0] > (2LL << (32 - maxzoom)) || sf.bbox[3] - sf.bbox[1] > (2LL << (32 - maxzoom))) {
		inline_meta = false;

		if (prevent[P_CLIPPING]) {
			static volatile long long warned = 0;
			long long extent = ((sf.bbox[2] - sf.bbox[0]) / ((1LL << (32 - maxzoom)) + 1)) * ((sf.bbox[3] - sf.bbox[1]) / ((1LL << (32 - maxzoom)) + 1));
			if (extent > warned) {
				fprintf(stderr, "Warning: %s:%d: Large unclipped (-pc) feature may be duplicated across %lld tiles\n", sst->fname, sst->line, extent);
				warned = extent;

				if (extent > 10000) {
					fprintf(stderr, "Exiting because this can't be right.\n");
					exit(EXIT_FAILURE);
				}
			}
		}
	}

	double extent = 0;
	if (additional[A_DROP_SMALLEST_AS_NEEDED]) {
		if (sf.t == VT_POLYGON) {
			for (size_t i = 0; i < sf.geometry.size(); i++) {
				if (sf.geometry[i].op == VT_MOVETO) {
					size_t j;
					for (j = i + 1; j < sf.geometry.size(); j++) {
						if (sf.geometry[j].op != VT_LINETO) {
							break;
						}
					}

					extent += get_area(sf.geometry, i, j);
					i = j - 1;
				}
			}
		} else if (sf.t == VT_LINE) {
			for (size_t i = 1; i < sf.geometry.size(); i++) {
				if (sf.geometry[i].op == VT_LINETO) {
					double xd = sf.geometry[i].x - sf.geometry[i - 1].x;
					double yd = sf.geometry[i].y - sf.geometry[i - 1].y;
					extent += sqrt(xd * xd + yd * yd);
				}
			}
		}
	}

	sf.extent = (long long) extent;

	if (!prevent[P_INPUT_ORDER]) {
		sf.seq = 0;
	}

	long long bbox_index;

	// Calculate the center even if off the edge of the plane,
	// and then mask to bring it back into the addressable area
	long long midx = (sf.bbox[0] / 2 + sf.bbox[2] / 2) & ((1LL << 32) - 1);
	long long midy = (sf.bbox[1] / 2 + sf.bbox[3] / 2) & ((1LL << 32) - 1);
	bbox_index = encode(midx, midy);

	if (additional[A_DROP_DENSEST_AS_NEEDED] || additional[A_CALCULATE_FEATURE_DENSITY] || additional[A_INCREASE_GAMMA_AS_NEEDED] || uses_gamma) {
		sf.index = bbox_index;
	} else {
		sf.index = 0;
	}

	if (inline_meta) {
		sf.metapos = -1;
		for (size_t i = 0; i < sf.full_keys.size(); i++) {
			sf.keys.push_back(addpool(sst->poolfile, sst->treefile, sf.full_keys[i].c_str(), mvt_string));
			sf.values.push_back(addpool(sst->poolfile, sst->treefile, sf.full_values[i].s.c_str(), sf.full_values[i].type));
		}
	} else {
		sf.metapos = *(sst->metapos);
		for (size_t i = 0; i < sf.full_keys.size(); i++) {
			serialize_long_long(sst->metafile, addpool(sst->poolfile, sst->treefile, sf.full_keys[i].c_str(), mvt_string), sst->metapos, sst->fname);
			serialize_long_long(sst->metafile, addpool(sst->poolfile, sst->treefile, sf.full_values[i].s.c_str(), sf.full_values[i].type), sst->metapos, sst->fname);
		}
	}

	long long geomstart = *(sst->geompos);
	serialize_feature(sst->geomfile, &sf, sst->geompos, sst->fname, *(sst->initial_x) >> geometry_scale, *(sst->initial_y) >> geometry_scale, false);

	struct index index;
	index.start = geomstart;
	index.end = *(sst->geompos);
	index.segment = sst->segment;
	index.seq = *(sst->layer_seq);
	index.t = sf.t;
	index.index = bbox_index;

	fwrite_check(&index, sizeof(struct index), 1, sst->indexfile, sst->fname);
	*(sst->indexpos) += sizeof(struct index);

	for (size_t i = 0; i < 2; i++) {
		if (sf.bbox[i] < sst->file_bbox[i]) {
			sst->file_bbox[i] = sf.bbox[i];
		}
	}
	for (size_t i = 2; i < 4; i++) {
		if (sf.bbox[i] > sst->file_bbox[i]) {
			sst->file_bbox[i] = sf.bbox[i];
		}
	}

	if (*(sst->progress_seq) % 10000 == 0) {
		checkdisk(sst->readers, CPUS);
		if (!quiet) {
			fprintf(stderr, "Read %.2f million features\r", *sst->progress_seq / 1000000.0);
		}
	}
	(*(sst->progress_seq))++;
	(*(sst->layer_seq))++;

	return 1;
}

void check_crs(json_object *j, const char *reading) {
	json_object *crs = json_hash_get(j, "crs");
	if (crs != NULL) {
		json_object *properties = json_hash_get(crs, "properties");
		if (properties != NULL) {
			json_object *name = json_hash_get(properties, "name");
			if (name->type == JSON_STRING) {
				if (strcmp(name->string, projection->alias) != 0) {
					fprintf(stderr, "%s: Warning: GeoJSON specified projection \"%s\", not the expected \"%s\".\n", reading, name->string, projection->alias);
					fprintf(stderr, "%s: If \"%s\" is not the expected projection, use -s to specify the right one.\n", reading, projection->alias);
				}
			}
		}
	}
}

void parse_json(struct serialization_state *sst, json_pull *jp, std::set<std::string> *exclude, std::set<std::string> *include, int exclude_all, int basezoom, int layer, double droprate, int maxzoom, std::map<std::string, layermap_entry> *layermap, std::string layername, bool uses_gamma, std::map<std::string, int> const *attribute_types, bool want_dist, bool filters) {
	long long found_hashes = 0;
	long long found_features = 0;
	long long found_geometries = 0;

	while (1) {
		json_object *j = json_read(jp);
		if (j == NULL) {
			if (jp->error != NULL) {
				fprintf(stderr, "%s:%d: %s\n", sst->fname, jp->line, jp->error);
				if (jp->root != NULL) {
					json_context(jp->root);
				}
			}

			json_free(jp->root);
			break;
		}

		if (j->type == JSON_HASH) {
			found_hashes++;

			if (found_hashes == 50 && found_features == 0 && found_geometries == 0) {
				fprintf(stderr, "%s:%d: Warning: not finding any GeoJSON features or geometries in input yet after 50 objects.\n", sst->fname, jp->line);
			}
		}

		json_object *type = json_hash_get(j, "type");
		if (type == NULL || type->type != JSON_STRING) {
			continue;
		}

		if (found_features == 0) {
			int i;
			int is_geometry = 0;
			for (i = 0; i < GEOM_TYPES; i++) {
				if (strcmp(type->string, geometry_names[i]) == 0) {
					is_geometry = 1;
					break;
				}
			}

			if (is_geometry) {
				if (j->parent != NULL) {
					if (j->parent->type == JSON_ARRAY) {
						if (j->parent->parent->type == JSON_HASH) {
							json_object *geometries = json_hash_get(j->parent->parent, "geometries");
							if (geometries != NULL) {
								// Parent of Parent must be a GeometryCollection
								is_geometry = 0;
							}
						}
					} else if (j->parent->type == JSON_HASH) {
						json_object *geometry = json_hash_get(j->parent, "geometry");
						if (geometry != NULL) {
							// Parent must be a Feature
							is_geometry = 0;
						}
					}
				}
			}

			if (is_geometry) {
				if (found_features != 0 && found_geometries == 0) {
					fprintf(stderr, "%s:%d: Warning: found a mixture of features and bare geometries\n", sst->fname, jp->line);
				}
				found_geometries++;

				serialize_geometry(sst, j, NULL, NULL, exclude, include, exclude_all, basezoom, layer, droprate, NULL, maxzoom, j, layermap, layername, uses_gamma, attribute_types, want_dist, filters);
				json_free(j);
				continue;
			}
		}

		if (strcmp(type->string, "Feature") != 0) {
			if (strcmp(type->string, "FeatureCollection") == 0) {
				check_crs(j, sst->fname);
				json_free(j);
			}

			continue;
		}

		if (found_features == 0 && found_geometries != 0) {
			fprintf(stderr, "%s:%d: Warning: found a mixture of features and bare geometries\n", sst->fname, jp->line);
		}
		found_features++;

		json_object *geometry = json_hash_get(j, "geometry");
		if (geometry == NULL) {
			fprintf(stderr, "%s:%d: feature with no geometry\n", sst->fname, jp->line);
			json_context(j);
			json_free(j);
			continue;
		}

		json_object *properties = json_hash_get(j, "properties");
		if (properties == NULL || (properties->type != JSON_HASH && properties->type != JSON_NULL)) {
			fprintf(stderr, "%s:%d: feature without properties hash\n", sst->fname, jp->line);
			json_context(j);
			json_free(j);
			continue;
		}

		json_object *tippecanoe = json_hash_get(j, "tippecanoe");
		json_object *id = json_hash_get(j, "id");

		json_object *geometries = json_hash_get(geometry, "geometries");
		if (geometries != NULL) {
			size_t g;
			for (g = 0; g < geometries->length; g++) {
				serialize_geometry(sst, geometries->array[g], properties, id, exclude, include, exclude_all, basezoom, layer, droprate, tippecanoe, maxzoom, j, layermap, layername, uses_gamma, attribute_types, want_dist, filters);
			}
		} else {
			serialize_geometry(sst, geometry, properties, id, exclude, include, exclude_all, basezoom, layer, droprate, tippecanoe, maxzoom, j, layermap, layername, uses_gamma, attribute_types, want_dist, filters);
		}

		json_free(j);

		/* XXX check for any non-features in the outer object */
	}
}

void *run_parse_json(void *v) {
	struct parse_json_args *pja = (struct parse_json_args *) v;

	parse_json(pja->jp, pja->reading, pja->layer_seq, pja->progress_seq, pja->metapos, pja->geompos, pja->indexpos, pja->exclude, pja->include, pja->exclude_all, pja->metafile, pja->geomfile, pja->indexfile, pja->poolfile, pja->treefile, pja->fname, pja->basezoom, pja->layer, pja->droprate, pja->file_bbox, pja->segment, pja->initialized, pja->initial_x, pja->initial_y, pja->readers, pja->maxzoom, pja->layermap, *pja->layername, pja->uses_gamma, pja->attribute_types, pja->dist_sum, pja->dist_count, pja->want_dist, pja->filters);

	return NULL;
}

struct jsonmap {
	char *map;
	unsigned long long off;
	unsigned long long end;
};

ssize_t json_map_read(struct json_pull *jp, char *buffer, size_t n) {
	struct jsonmap *jm = (struct jsonmap *) jp->source;

	if (jm->off + n >= jm->end) {
		n = jm->end - jm->off;
	}

	memcpy(buffer, jm->map + jm->off, n);
	jm->off += n;

	return n;
}

struct json_pull *json_begin_map(char *map, long long len) {
	struct jsonmap *jm = new jsonmap;
	if (jm == NULL) {
		perror("Out of memory");
		exit(EXIT_FAILURE);
	}

	jm->map = map;
	jm->off = 0;
	jm->end = len;

	return json_begin(json_map_read, jm);
}

void json_end_map(struct json_pull *jp) {
	delete (struct jsonmap *) jp->source;
	json_end(jp);
}
