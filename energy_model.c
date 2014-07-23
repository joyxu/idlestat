#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>

#include "idlestat.h"
#include "topology.h"
#include "list.h"
#include "utils.h"

static char buffer[BUFSIZE];

static struct cluster_energy_info *cluster_energy_table;
static unsigned int clusters_in_energy_file = 0;

static int make_energy_model_template(const char *path, const char *trace_file_path)
{
	FILE *f;
	struct cpuidle_datas *datas;
	struct cpu_physical *s_phy;
	struct cpu_core     *s_core;
	struct cpu_cpu      *s_cpu;
	unsigned int cluster_count = 0;
	unsigned int cluster_number = 0;

	f = fopen(path, "w+");
	if (!f)
		return -1;
	fprintf(f, "# This is a energy model template generated by idlestat\n");
	fprintf(f, "# Lines starting with # or which are blank are ignored\n");
	fprintf(f, "# Replace ? with correct values\n");

	init_cpu_topo_info();
	read_sysfs_cpu_topo();
	datas = idlestat_load(trace_file_path);
	if (!datas)
		return -1;
	establish_idledata_to_topo(datas);

	list_for_each_entry(s_phy, &g_cpu_topo_list.physical_head,
			    list_physical) {
		cluster_count++;
	}
	fprintf(f, "clusters %d\n", cluster_count);

	list_for_each_entry(s_phy, &g_cpu_topo_list.physical_head,
			    list_physical) {
		unsigned int num_cap_states = 0;
		unsigned int num_c_states = 0;
		unsigned int i;

		s_core = list_entry((&s_phy->core_head)->prev, struct cpu_core, list_core);
		s_cpu = list_entry((&s_core->cpu_head)->prev, struct cpu_cpu, list_cpu);
		num_cap_states = s_cpu->pstates->max;
		num_c_states = s_cpu->cstates->cstate_max;

		fprintf(f, "cluster%c: %d cap states %d C states\n\n", cluster_number + 'A', num_cap_states, num_c_states);
		fprintf(f, "P-states:\n");
		fprintf(f, "# speed, cluster power, core power\n");

		for (i = 0; i < s_cpu->pstates->max; i++) {
			struct cpufreq_pstate *p = &s_cpu->pstates->pstate[i];

			fprintf(f, "%d\t\t?\t?\n", p->freq/1000);
		}
		fprintf(f, "\nC-states:\n");
		fprintf(f, "# name, cluster power, core power\n");
		for (i = 0; i < s_cpu->cstates->cstate_max + 1; i++) {
			struct cpuidle_cstate *c = &s_cpu->cstates->cstate[i];

			fprintf(f, "%s\t\t?\t?\n", c->name);
		}
		fprintf(f, "\nwakeup\t\t?\t?\n");
		cluster_number++;
	}
	return 0;
}

int parse_energy_model(const char *path, const char *trace_file_path)
{
	FILE *f;
	char tmp;
	struct cluster_energy_info *clustp;
	unsigned int number_cap_states, number_c_states;
	int current_cluster = -1;
	unsigned int current_pstate;
	unsigned int current_cstate;
	unsigned int clust_p, core_p;
	int ret;

	f = fopen(path, "r");
	if (!f) {
		if (errno == ENOENT) {
			ret = make_energy_model_template(path, trace_file_path);
			exit(ret);
		}
		fprintf(stderr, "%s: failed to open '%s': %m\n", __func__, path);
		return -1;
	}

	fgets(buffer, BUFSIZE, f);

	do {
		if (buffer[0] == '#') continue;
		if (strlen(buffer) == 1) continue;

		if (strstr(buffer, "clusters")) {
			if (clusters_in_energy_file) {
				fprintf(stderr, "%s: number of clusters already specified in %s\n", __func__, path);
				return -1;
			}
			sscanf(buffer, "%*s %d", &clusters_in_energy_file);
			cluster_energy_table = calloc(sizeof(struct cluster_energy_info), clusters_in_energy_file);
			continue;
		}
		if (strstr(buffer, "cluster")) {
			sscanf(buffer, "cluster%c: %d cap states %d C states", &tmp, &number_cap_states, &number_c_states);
			current_cluster = tmp - 'A';
			if (current_cluster >= clusters_in_energy_file) {
				fprintf(stderr, "%s: cluster%c out of range in %s\n", __func__, tmp, path);
				return -1;
			}
			clustp = cluster_energy_table + current_cluster;
			if (clustp->number_cap_states) {
				fprintf(stderr, "%s: number of cap states for cluster%c already specified in %s\n", __func__, tmp, path);
				return -1;
			}
			clustp->number_cap_states = number_cap_states;
			clustp->number_c_states = number_c_states;
			clustp->p_energy = calloc(number_cap_states, sizeof(struct pstate_energy_info));
			clustp->c_energy = calloc(number_c_states, sizeof(struct cstate_energy_info));
			clustp->state = parsed_cluster_info;
			continue;
		}
		if (strstr(buffer, "P-states")) {
			if (current_cluster == -1) {
				fprintf(stderr, "%s: unknown cluster (cap states) in %s\n", __func__, path);
				return -1;
			}
			if (clustp->state < parsed_cluster_info) {
				fprintf(stderr, "%s: number of cap states for cluster%c not specified in %s\n", __func__, current_cluster, path);
				return -1;
			}
			current_pstate = 0;
			clustp->state = parsing_cap_states;
			continue;
		}
		if (strstr(buffer, "C-states")) {
			if (current_cluster == -1) {
				fprintf(stderr, "%s: unknown cluster (c states) in %s\n", __func__, path);
				return -1;
			}
			if (clustp->state < parsed_cluster_info) {
				fprintf(stderr, "%s: number of c states for cluster%c not specified in %s\n", __func__, current_cluster, path);
				return -1;
			}
			current_cstate = 0;
			clustp->state = parsing_c_states;
			continue;
		}
		if (strstr(buffer, "wakeup")) {
			unsigned int clust_w, core_w;

			if (current_cluster == -1) {
				fprintf(stderr, "%s: unknown cluster (wakeup) in %s\n", __func__, path);
				return -1;
			}
			sscanf(buffer, "%*s %d %d", &clust_w, &core_w);
			clustp->wakeup_energy.cluster_wakeup_energy = clust_w;
			clustp->wakeup_energy.core_wakeup_energy = core_w;
			continue;
		}
		if (!clustp) {
			fprintf(stderr, "%s: unknown cluster in %s\n", __func__, path);
			return -1;
			}
		if (clustp->state == parsing_cap_states) {
			struct pstate_energy_info *pp;
			unsigned int speed;

			if (sscanf(buffer, "%d %d %d", &speed, &clust_p, &core_p) != 3) {
				fprintf(stderr, "%s: expected P state (speed cluster core) for cluster%c in %s\n", __func__, current_cluster, path);
				return -1;
			}
			
			if (current_pstate >= clustp->number_cap_states) {
				fprintf(stderr, "%s: too many cap states specified for cluster%c in %s\n", __func__, current_cluster, path);
				return -1;
			}
			pp = &clustp->p_energy[current_pstate++];
			pp->speed = speed;
			pp->cluster_power = clust_p;
			pp->core_power = core_p;
			continue;
		}
		if (clustp->state == parsing_c_states) {
			char name[NAMELEN];
			struct cstate_energy_info *cp;

			if (sscanf(buffer, "%s %d %d", name, &clust_p, &core_p) != 3) {
				fprintf(stderr, "%s: expected C state (name cluster core) for cluster%c in %s\n", __func__, current_cluster, path);
				return -1;
			}

			if (current_cstate >= clustp->number_c_states) {
				fprintf(stderr, "%s: too many C states specified for cluster%c in %s\n", __func__, current_cluster, path);
				return -1;
			}
			cp = &clustp->c_energy[current_cstate++];
			strncpy(cp->cstate_name, name, NAMELEN);
			cp->cluster_idle_power = clust_p;
			cp->core_idle_power = core_p;
			continue;
		}
	} while (fgets(buffer, BUFSIZE, f));

	printf("parsed energy model file\n");
	return 0;
}

static struct cstate_energy_info *find_cstate_energy_info(const unsigned int cluster, const char *name)
{
	struct cluster_energy_info *clustp;
	struct cstate_energy_info *cp;
	int i;

	clustp = cluster_energy_table + cluster;
	cp = &clustp->c_energy[0];
	for (i = 0; i < clustp->number_c_states; i++, cp++) {
		if (!strcmp(cp->cstate_name, name)) return cp;
	}
	return NULL;
}

static struct pstate_energy_info *find_pstate_energy_info(const unsigned int cluster, const unsigned int speed)
{
	struct cluster_energy_info *clustp;
	struct pstate_energy_info *pp;
	int i;

	clustp = cluster_energy_table + cluster;
	pp = &clustp->p_energy[0];
	for (i = 0; i < clustp->number_cap_states; i++, pp++) {
		if (speed == pp->speed) return pp;
	}
	return NULL;
}

#define US_TO_SEC(US) (US / 1e6)

void calculate_energy_consumption(void)
{
	struct cpu_physical *s_phy;
	struct cpu_core     *s_core;
	struct cpu_cpu      *s_cpu;
	double total_energy_used = 0.0;
	double energy_from_cap_states = 0.0;
	double energy_from_idle = 0.0;
	double energy_from_wakeups = 0.0;
	int i, j;
	unsigned int current_cluster;
	struct cstate_energy_info *cp;
	struct pstate_energy_info *pp;
	struct cluster_energy_info *clustp;

	/* Contributions are computed per Cluster */

	list_for_each_entry(s_phy, &g_cpu_topo_list.physical_head,
			    list_physical) {
		current_cluster = s_phy->physical_id;
		clustp = cluster_energy_table + current_cluster;

		print_vrb("\n\nCluster%c%25s | %13s | %7s | %7s | %12s | %12s | %12s |\n",
				'A'+current_cluster, "", "[us] Duration", "Power", "Energy", "E_cap", "E_idle", "E_wkup");

		/* All C-States on current Cluster */

		for (j = 0; j < s_phy->cstates->cstate_max + 1; j++) {
			struct cpuidle_cstate *c = &s_phy->cstates->cstate[j];

			if (c->nrdata == 0) {
				print_vrb("      C%d no data for [%s]\n",
						j, c->name);
				continue;
			}

			cp = find_cstate_energy_info(current_cluster, c->name);
			if (!cp) {
				print_vrb("      C%d no energy model for [%s] (%d hits, %f duration)\n",
						j, c->name, c->nrdata, c->duration);
				continue;
			}

			/* Cluster WU energy: defined just for wakeups from C1 */
			if (strcmp(c->name, "C1") == 0) {

				energy_from_wakeups += c->nrdata * clustp->wakeup_energy.cluster_wakeup_energy;

				print_vrb("      C%d +%7d wkps frm [%4s] | %13s | %7s | %7d | %12s | %12s | %12.0f |\n",
					j, c->nrdata, c->name,
					"", "",
					clustp->wakeup_energy.cluster_wakeup_energy,
					"", "",
					energy_from_wakeups);
			}

			/* NOTE: this keeps track of just the last C-State
			 * duration... are you assuming this is the C1
			 * residency time ?!? */
			/* The filtered trace is now accounting properly all the
			 * intervals in wich a CPUs is asking for C1 but it is
			 * actually only being in WFI. Since a WFI event is
			 * genarated in that case, it is not more qequired to
			 * compute that funny:
			 *    total_WFI - C1 on which cluster is not in C1
			 */
			energy_from_idle += c->duration * cp->cluster_idle_power;

			print_vrb("      C%d +%7d hits for [%4s] | %13.0f | %7d | %7s | %12s | %12.0f | %12s |\n",
					j, c->nrdata, c->name,
					c->duration,
					cp->cluster_idle_power,
					"", "",
					energy_from_idle,
					"");

		}

		/* Add current cluster wakeup energy contribution */
		/* This assumes that cluster wakeup hits equal the number of cluster C-States enter */

		/* All C-States and P-States for the CPUs on current Cluster */
		list_for_each_entry(s_core, &s_phy->core_head, list_core) {

			/* On big.LITTLE every Core has just one CPU */

			list_for_each_entry(s_cpu, &s_core->cpu_head,
					    list_cpu) {

				/* All C-States of current CPU */

				for (i = 0; i < s_cpu->cstates->cstate_max + 1; i++) {
					struct cpuidle_cstate *c = &s_cpu->cstates->cstate[i];
					if (c->nrdata == 0) {
						print_vrb("Cpu%d, C%d no data for [%s]\n",
							s_cpu->cpu_id, i, c->name);
						continue;
					}
					cp = find_cstate_energy_info(current_cluster, c->name);
					if (!cp) {
						print_vrb("Cpu%d, C%d no energy model for [%s] (%d hits, %f duration)\n",
							s_cpu->cpu_id, i, c->name,
							c->nrdata, c->duration);
						continue;
					}
					energy_from_idle += c->duration * cp->core_idle_power;

					print_vrb("Cpu%d  C%d +%7d hits for [%4s] | %13.0f | %7d | %7s | %12s | %12.0f | %12s |\n",
							s_cpu->cpu_id, i, c->nrdata, c->name,
							c->duration,
							cp->core_idle_power,
							"", "",
							energy_from_idle,
							"");

					/* CPU WU energy: defined just for wakeups from WFI (on filtered trace) */
					if (strcmp(c->name, "WFI") == 0) {

						energy_from_wakeups += c->nrdata * clustp->wakeup_energy.core_wakeup_energy;

						print_vrb("Cpu%d  C%d +%6d wkps frm [%4s] | %13s | %7s | %7d | %12s | %12s | %12.0f |\n",
								s_cpu->cpu_id, i, c->nrdata, c->name,
								"", "",
								clustp->wakeup_energy.core_wakeup_energy,
								"", "",
								energy_from_idle);
					}


				}

				/* All P-States of current CPU */

				for (i = 0; i < s_cpu->pstates->max; i++) {
					struct cpufreq_pstate *p = &s_cpu->pstates->pstate[i];

					if (p->count == 0) {
						print_vrb("Cpu%d, P%d no data for [%d]\n",
							s_cpu->cpu_id, i, p->freq/1000);
						continue;
					}
					pp = find_pstate_energy_info(current_cluster, p->freq/1000);
					if (!pp) {
						print_vrb("Cpu%d, P%d no energy model for [%d] (%d hits, %f duration)\n",
							s_cpu->cpu_id, i, p->freq/1000,
							p->count, p->duration);
						continue;
					}
					pp->max_core_duration = MAX(p->duration, pp->max_core_duration);
					energy_from_cap_states += p->duration * pp->core_power;

					print_vrb("Cpu%d  P%d +%7d hits for [%4d] | %13.0f | %7d | %7s | %12.0f | %12s | %12s |\n",
							s_cpu->cpu_id, i, p->count, p->freq/1000,
							p->duration, pp->core_power,
							"",
							energy_from_cap_states,
							"", "");
				}
			}
		}
		/*
		 * XXX
		 * No cluster P-state duration info available yet, so estimate this
		 * as the maximum of the durations of its cores at that frequency.
		 */
		for (i = 0; i < clustp->number_cap_states; i++) {
			pp = &clustp->p_energy[i];
			energy_from_cap_states += pp->max_core_duration * pp->cluster_power;

			print_vrb("      P%d casp estimate for [%4d] | %13.0f | %7d | %7s | %12.0f | %12s | %12s |\n",
					clustp->number_cap_states - i - 1, pp->speed,
					pp->max_core_duration,
					pp->cluster_power,
					"",
					energy_from_cap_states,
					"", "");
		}
	}

	/* NOTE: wakeups are accounted in hits only, since the time
	 *       componente is assumed to be constant for each hit and
	 *       must be added by scaling 'E_wu' by:
	 *          LOAD_AVG_MAX/1024 = (47742/1024)
	 *       where
	 *          LOAD_AVG_MAX == maximum possible load avg
	 */
	energy_from_wakeups *= 47742;
	energy_from_wakeups /= 1024;

	printf("\n");
	/* Convert all [us] components to [s] just here to avoid summing
	 * truncation errors due to small components */
	printf("energy consumption from cap states \t%14.0f (%e)\n",
			US_TO_SEC(energy_from_cap_states),
			US_TO_SEC(energy_from_cap_states));
	printf("energy consumption from idle \t\t%14.0f (%e)\n",
			US_TO_SEC(energy_from_idle),
			US_TO_SEC(energy_from_idle));
	printf("energy consumption from wakeups \t%14.0f (%e)\n",
			energy_from_wakeups,
			energy_from_wakeups);
	total_energy_used =
		US_TO_SEC(energy_from_cap_states) +
		US_TO_SEC(energy_from_idle) +
		energy_from_wakeups;
	printf("\ntotal energy consumption estimate \t%14.0f (%e)\n\n\n",
			total_energy_used,
			total_energy_used);
}
