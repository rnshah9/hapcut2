#include<stdio.h>
#include<stdlib.h>
#include<time.h>
#include<math.h>
#include<string.h>

// data structures and global variables 
#include "common.h" // datastructures for fragments, variants and haplotype blocks
#include "fragments.h" // fragment likelihood, haplotype assignment
#include "variantgraph.h" // 
#include "hapcontig.h" // haplotype contigs or blocks 
#include "optionparser.c" // global variables and parse command line arguments to change these variables 

// input related
#include "readinputfiles.h" // read fragment matrix generated by extracthairs
#include "readvcf.h" // read VCF file 
//#include "data_stats.c"

// maxcut optimization related
#include "pointerheap.h"  // heap for max-cut greedy algorithm
#include "find_maxcut.c"   // function compute_good_cut
#include "post_processing.c"  // post-process haplotypes to remove SNVs with low-confidence phasing, split blocks 
#include "hic.h"  // HiC relevant functions

// output related
#include "output_phasedvcf.c" // output VCF phased file

// IMPORTANT NOTE: all SNPs start from 1 instead of 0 and all offsets are 1+ in fragment file

// captures all the relevant data structures for phasing as a single structure
typedef struct    
{
	struct fragment* Flist; int fragments;
	struct SNPfrags* snpfrag; int snps;
	char* HAP1;
	struct BLOCK* clist; int components;
} DATA;


int detect_long_reads(struct fragment* Flist,int fragments)
{
    int i=0;
    int long_reads=0;
    float mean_snps_per_read = 0;
    if (AUTODETECT_LONGREADS){
        for (i = 0; i < fragments; i++){
            mean_snps_per_read += Flist[i].calls;
        }
        mean_snps_per_read /= fragments;
        if (mean_snps_per_read >= 3){
            long_reads = 1;
        }else{
            long_reads = 0;
        }
    }
    fprintf(stderr,"mean number of variants per read is %0.2f \n",mean_snps_per_read);
    return long_reads;
}

int read_input_files(char* fragmentfile,char* fragmentfile2,char* variantfile,DATA* data)
{
    // READ FRAGMENT MATRIX, allows for second fragment file as well. 
    data->fragments = get_num_fragments(fragmentfile);
    int offset = data->fragments;
    if (strcmp(fragmentfile2,"None") !=0) data->fragments += get_num_fragments(fragmentfile2);
    data->Flist = (struct fragment*) malloc(sizeof (struct fragment)* data->fragments);
    int flag = read_fragment_matrix(fragmentfile, data->Flist,offset,0);
    if (strcmp(fragmentfile2,"None") !=0) read_fragment_matrix(fragmentfile2, data->Flist,data->fragments-offset,offset);

    int new_fragments = 0;
    struct fragment* new_Flist;
    int i=0;

    if (MAX_IS != -1){
        // we are going to filter out some insert sizes, is some memory being lost here?
        new_fragments = 0;
        new_Flist = (struct fragment*) malloc(sizeof (struct fragment)* data->fragments);
        for(i = 0; i < data->fragments; i++){
            if (data->Flist[i].isize < MAX_IS) new_Flist[new_fragments++] = data->Flist[i];
        }
        data->Flist = new_Flist;
        data->fragments = new_fragments;
    }
    if (flag < 0) {
        fprintf_time(stderr, "unable to read fragment matrix file %s \n", fragmentfile);
        return 0;
    }
    data->snps = count_variants_vcf(variantfile);
    if (data->snps < 0) {
        fprintf_time(stderr, "unable to read variant file %s \n", variantfile);
        return 0;
    }
    else // read VCF file
    {
	   data->snpfrag = (struct SNPfrags*) malloc(sizeof (struct SNPfrags)*data->snps);
    	   read_vcffile(variantfile, data->snpfrag, data->snps);
    }
    return 1;	
} 

void init_random_hap(struct SNPfrags* snpfrag,int snps,char* HAP1)
{
    int i=0;
    for (i = 0; i < snps; i++) 
    {
	// this should be checked only after fragments per SNV have been counted
        if (snpfrag[i].frags == 0 || (SNVS_BEFORE_INDELS && (strlen(snpfrag[i].allele0) != 1 || strlen(snpfrag[i].allele1) != 1)) || snpfrag[i].ignore == '1') 
        {
            HAP1[i] = '-';  
        } 
        else if (drand48() < 0.5) HAP1[i] = '0';
        else HAP1[i] = '1';
    }
}

void free_memory(struct SNPfrags* snpfrag,int snps,struct BLOCK* clist,int components)
{
    int i=0;
    for (i = 0; i < snps; i++) free(snpfrag[i].elist);
    for (i = 0; i < snps; i++) free(snpfrag[i].telist);
    int component = 0;
    for (i = 0; i < snps; i++) {
        free(snpfrag[i].flist);
        free(snpfrag[i].alist);
        free(snpfrag[i].jlist);
        free(snpfrag[i].klist);

        if (snpfrag[i].component == i && snpfrag[i].csize > 1) // root node of component
        {
            free(clist[component].slist);
            component++;
        }
    }
    for (i = 0; i < components; i++) free(clist[i].flist);
}

void print_output_files(DATA* data,char* variantfile, char* outputfile)
{

    fprintf_time(stderr, "OUTPUTTING PRUNED HAPLOTYPE ASSEMBLY TO FILE %s\n", outputfile);
    print_contigs(data->clist,data->components,data->HAP1,data->Flist,data->fragments,data->snpfrag,outputfile);

    char assignfile[4096];  sprintf(assignfile,"%s.tags",outputfile);
    if (OUTPUT_HAPLOTAGS ==1) fragment_assignments(data->Flist,data->fragments,data->snpfrag,data->HAP1,assignfile); // added 03/10/2018 to output read-haplotype assignments

    char outvcffile[4096];  sprintf(outvcffile,"%s.phased.VCF",outputfile);
    if (OUTPUT_VCF ==1) {
    	fprintf_time(stderr, "OUTPUTTING PHASED VCF TO FILE %s\n", outvcffile);
	output_vcf(variantfile,data->snpfrag,data->snps,data->HAP1,data->Flist,data->fragments,outvcffile,0);
    }
}

void optimization_using_maxcut(DATA* data)
{
    int iter=0; int hic_iter=0; int k=0; int i=0;
    int* slist = (int*) malloc(sizeof (int)*data->snps);
    int converged_count=0;

    // compute the component-wise score for 'initHAP' haplotype
    float miscalls = 0;
    float bestscore = 0;
    for (k = 0; k < data->components; k++) {
        data->clist[k].SCORE = 0;
        data->clist[k].bestSCORE = 0;
        for (i = 0; i < data->clist[k].frags; i++) {
            update_fragscore1(data->Flist, data->clist[k].flist[i],data->HAP1);
            data->clist[k].SCORE += data->Flist[data->clist[k].flist[i]].currscore;
        }
        data->clist[k].bestSCORE = data->clist[k].SCORE;
        bestscore += data->clist[k].bestSCORE;
        miscalls += data->clist[k].SCORE;
    }

    HTRANS_MAXBINS = 0;
    if (HIC) init_HiC(data->Flist,data->fragments,HTRANS_DATA_INFILE);
    float HIC_LL_SCORE = -80;
    float OLD_HIC_LL_SCORE = -80;

    OLD_HIC_LL_SCORE = bestscore;
    for (hic_iter = 0; hic_iter < MAX_HIC_EM_ITER; hic_iter++){ // single iteration except for HiC 
        if (VERBOSE)
            fprintf_time(stdout, "HIC ITER %d\n", hic_iter);
        for (k = 0; k < data->components; k++){
            data->clist[k].iters_since_improvement = 0;
        }
        for (i=0; i<data->snps; i++){
            data->snpfrag[i].post_hap = 0;
        }
        // RUN THE MAX_CUT ALGORITHM ITERATIVELY TO IMPROVE LIKELIHOOD
        for (iter = 0; iter < MAXITER; iter++) {
            if (VERBOSE)
                fprintf_time(stdout, "PHASING ITER %d\n", iter);
            converged_count = 0;
            for (k = 0; k < data->components; k++){
                if(VERBOSE && iter == 0)
                    fprintf_time(stdout, "component %d length %d phased %d %d...%d\n", k, data->clist[k].length, data->clist[k].phased,data->clist[k].offset,data->clist[k].lastvar);
                if (data->clist[k].SCORE > 0)
                    converged_count += evaluate_cut_component(data->Flist,data->snpfrag,data->clist, k, slist, data->HAP1);
                else converged_count++;
            }

            if (converged_count == data->components) {
                //fprintf(stdout, "Haplotype assembly terminated early because no improvement seen in blocks after %d iterations\n", CONVERGE);
                break;
            }
        }

        // H-TRANS ESTIMATION FOR HIC
        if (MAX_HIC_EM_ITER > 1){

            // Possibly break if we're done improving
            HIC_LL_SCORE = 0;
            for (k = 0; k < data->components; k++){
                HIC_LL_SCORE += data->clist[k].bestSCORE;
            }
            if (HIC_LL_SCORE >= OLD_HIC_LL_SCORE){
                break;
            }
            OLD_HIC_LL_SCORE = HIC_LL_SCORE;

            likelihood_pruning(data->snps,data->Flist,data->snpfrag,data->HAP1, 0); // prune for only very high confidence SNPs
            // estimate the h-trans probabilities for the next round
            estimate_htrans_probs(data->Flist,data->fragments,data->HAP1,data->snpfrag,HTRANS_DATA_OUTFILE);
        }
    }
    free(slist);
}

int build_readvariant_graph(DATA* data)
{
    int i=0; int components=0;
    LONG_READS = detect_long_reads(data->Flist,data->fragments);
    for (i=0;i<data->snps;i++)
    {
	// ignore homzygous variants, for such variants, the HAP1[i] value is set to '-' to avoid using them for phasing  
	if (data->snpfrag[i].genotypes[0] == data->snpfrag[i].genotypes[2]) data->snpfrag[i].ignore = '1'; 
	else data->snpfrag[i].ignore = '0'; 
    }
    update_snpfrags(data->Flist, data->fragments, data->snpfrag, data->snps);
    // 10/25/2014, edges are only added between adjacent nodes in each fragment and used for determining connected components...
   // for (i = 0; i < snps; i++) snpfrag[i].elist = (struct edge*) malloc(sizeof (struct edge)*(snpfrag[i].edges+1)); // # of edges calculated in update_snpfrags function 
    if (LONG_READS ==0)  add_edges(data->Flist,data->fragments,data->snpfrag,data->snps,&components);
    else if (LONG_READS >=1) add_edges_longreads(data->Flist,data->fragments,data->snpfrag,data->snps,&components);
    // length of telist is smaller since it does not contain duplicates, calculated in add_edges 
    for (i = 0; i < data->snps; i++) data->snpfrag[i].telist = (struct edge*) malloc(sizeof (struct edge)*(data->snpfrag[i].edges+1));
    data->components = components;
}

void post_processing(DATA* data)
{
    // BLOCK SPLITTING
    int split_count, new_components;
    int k=0;
    new_components = data->components;

    if (SPLIT_BLOCKS){
        split_count = 0;
        for (k=0; k<data->components; k++){
            split_count += split_block(data->HAP1, data->clist,k, data->Flist, data->snpfrag, &new_components); // attempt to split block
        }
        if (split_count > 0){
            // regenerate clist if necessary
            free(data->clist);
            data->clist = (struct BLOCK*) malloc(sizeof (struct BLOCK)*new_components);
            generate_contigs(data->Flist, data->fragments, data->snpfrag, data->snps, new_components, data->clist);
        }
        data->components = new_components;
    }else if(ERROR_ANALYSIS_MODE && !HIC){
        for (k=0; k<data->components; k++){
            // run split_block but don't actually split, just get posterior probabilities
            split_block(data->HAP1, data->clist, k, data->Flist, data->snpfrag, &new_components);
        }
    }

    // PRUNE INDIVIDUAL VARIANTS based on phased genotype likelihoods
    if (!SKIP_PRUNE){
        //discrete_pruning(snps, fragments, Flist, snpfrag, HAP1);
	//if (UNPHASED ==1) unphased_optim(snps,Flist,snpfrag,HAP1);
        likelihood_pruning(data->snps,data->Flist, data->snpfrag,data->HAP1, CALL_HOMOZYGOUS);
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

int maxcut_haplotyping(char* fragmentfile,char* fragmentfile2, char* variantfile, char* outputfile) {

    // READ INPUT FILES (Fragments and Variants)
    DATA data; 
    if (read_input_files(fragmentfile,fragmentfile2,variantfile,&data) < 1)  return -1;
    fprintf_time(stderr, "processed fragment file and variant file: fragments %d variants %d\n",data.fragments,data.snps);
    struct fragment* Flist = data.Flist; int fragments = data.fragments;
    struct SNPfrags* snpfrag =data.snpfrag; int snps = data.snps;

    // BUILD FRAGMENT-VARIANT GRAPH 
    fprintf_time(stderr, "building read-variant graph for phasing\n");
    build_readvariant_graph(&data); 
    
    // INITIALIZE HAPLOTYPES
    data.HAP1 = (char*) malloc(snps + 1);
    init_random_hap(snpfrag,snps,data.HAP1);

    // BUILD CONTIGS/CONNECTED COMPONENTS OF GRAPH 
    data.clist = (struct BLOCK*) malloc(sizeof (struct BLOCK)*data.components);
    generate_contigs(data.Flist, data.fragments, data.snpfrag, data.snps,data.components,data.clist);
    fprintf_time(stderr, "fragments %d snps %d component(blocks) %d\n", data.fragments, snps,data.components);

    // MAX-CUT optimization
    fprintf_time(stderr, "starting Max-Likelihood-Cut based haplotype assembly algorithm\n");
    optimization_using_maxcut(&data);

    // POST PROCESSING OF HAPLOTYPES 
    fprintf_time(stderr, "starting to post-process phased haplotypes to further improve accuracy\n");
    post_processing(&data);

    // PRINT OUTPUT FILES
    fprintf_time(stderr, "starting to output phased haplotypes\n");
    print_output_files(&data,variantfile,outputfile);

    // FREE DATA STRUCTURES
    free_memory(data.snpfrag,snps,data.clist,data.components); 
    // need to free memory used by each fragment individually... not done
    free(data.Flist);
    free(data.snpfrag);
    free(data.clist);
    return 0;
}

int main(int argc, char** argv) {
    int i = 0;
    //char* fragfile = NULL; char* fragfile2 = NULL; char* VCFfile = NULL; char* hapfile = NULL;
    char fragfile[10000]; char fragfile2[10000];
    strcpy(fragfile, "None");   strcpy(fragfile2, "None"); 
    char VCFfile[10000]; char hapfile[10000];
    strcpy(VCFfile, "None"); strcpy(hapfile, "None");
    strcpy(HTRANS_DATA_INFILE, "None"); strcpy(HTRANS_DATA_OUTFILE, "None");

    parse_arguments(argc,argv,fragfile,fragfile2,VCFfile,hapfile);
    maxcut_haplotyping(fragfile,fragfile2,VCFfile, hapfile);

    return 0;
}
