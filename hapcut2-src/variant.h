#ifndef _VARIANT_H
#define _VARIANT_H
#include <stdint.h>
#include<stdio.h>
#include <math.h>
#include<stdlib.h>
#include<time.h>
#include<string.h>
#include<ctype.h>

struct SNPfrags { // single variant, also holds all information for node in graph 
    int* flist;
    int* jlist; // list of j indexes used to index into Flist[f].list
    int* klist; // list of k indexes used to index into Flist[f].list[j]
    int frags; // number of fragments covering this vairant 
    char* alist; // alist is the alleles corresponding to each fragment that covers this SNP
    int component;
    int edges; // those that span the interval between this snp and the next snp
    int csize;
    struct edge* elist;
    int bcomp; // index of clist to which this snp belongs: reverse mapping
    struct edge* telist;
    int tedges; // temporary edge list and number of edges for MIN CUT computation
    int parent;
    float score;
    float htscore; // htrans
    int heaploc;

    // changed on feb 1 2012 to be pointers (char* id, char* chrom)
    char* chromosome;
    int position;
    char* id;
    char* allele0;
    char* allele1;
    char* genotypes; // VCF genotypes 0|1 1|0 or 0/1 added feb 1 2012

    char  ignore; // ignore this variant for all hapcut computations, 12/17/2018
    float post_notsw;
    float post_hap;
    int pruned_discrete_heuristic; // for error analysis mode
    float homozygous_prior; // prior probability of homozygousity. Based on GQ field of VCF.
    float PGLL[5]; // phased genotype likelihoods, 00,01,10,11 and ./. (if variant is ignored) 
    float hetLL; // unphased heterozygous 0/1 likelihood of genotype
};

#endif
