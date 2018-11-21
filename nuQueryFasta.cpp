#include "nuQueryFasta.h"

#include "stdlib.h"
#include <vector>
#include <string>
#include <iostream>
#include <fstream>
#include <numeric>
#include <pthread.h>
#include <semaphore.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <cassert>
#include <cstring>
#include <algorithm>

using namespace std;

sem_t *semreader;
sem_t *semcount;
sem_t *semfastqwriter;

void rand_str(char *dest, size_t length) {
    char charset[] = "0123456789"
                     "abcdefghijklmnopqrstuvwxyz"
                     "ABCDEFGHIJKLMNOPQRSTUVWXYZ";		
    while (length-- > 0) {
        size_t index = (double) rand() / RAND_MAX * (sizeof charset - 1);
        *dest++ = charset[index];
    }
    *dest = '\0';
}

tuple<size_t, size_t> countHit(kmerCount_dict &kmers, kmerIndex_dict &kmerDBi, size_t nloci) {
    vector<size_t> hits(nloci);
    for (auto &p : kmers) {
	if (kmerDBi.count(p.first) == 1) {
	    for (size_t i : kmerDBi[p.first]) {
		hits[i] += p.second;
	    }	
	}
    }
    vector<size_t>::iterator it = max_element(hits.begin(), hits.end());
    return make_tuple(*it, distance(hits.begin(), it));
}

class Counts {
public:
    ifstream *in;
    bool ftype;
    kmerIndex_dict *kmerDBi;
    size_t *readIndex;
    size_t threadIndex, k, nloci, threshold;
    vector<kmerCount_dict> queryResults;

    Counts(size_t nloci_) : queryResults(nloci_), nloci(nloci_) {}
};

class Threads {
public:
    vector<Counts> counts;
    Threads(size_t nproc, size_t nloci) : counts(nproc, Counts(nloci)) {}
};

size_t readNumber = 0;
size_t nHits = 0;

void CountWords(void *data) {
    kmerIndex_dict &kmerDBi = *((Counts*)data)->kmerDBi;
    size_t &readNumber = *((Counts*)data)->readIndex;
    ifstream *in = ((Counts*)data)->in;
    bool ftype = ((Counts*)data)->ftype;
    size_t k = ((Counts*)data)->k;
    vector<kmerCount_dict> &queryResults = ((Counts*)data)->queryResults;
    size_t threadIndex = ((Counts*)data)->threadIndex;
    size_t nloci = ((Counts*)data)->nloci;
    size_t threshold = ((Counts*)data)->threshold;

    while (true) {
        //
        // begin thread locking
        //
        sem_wait(semreader);

        assert((*in).good() == true);
        string title, title1, seq, seq1, qualtitle, qualtitle1, qual, qual1;
        size_t readn = 0;
        vector<string> seqs;

        if (ftype) {
            while (readn <= 150000 and (*in)) {
                getline(*in, title);
                getline(*in, seq);
                getline(*in, qualtitle);
                getline(*in, qual);
                getline(*in, title1);
                getline(*in, seq1);
                getline(*in, qualtitle1);
                getline(*in, qual1);

                size_t start = 0;
                size_t len = seq.size();
                while (qual[start] == '#'){ start++; len--; }
                while (qual[start + len - 1] == '#'){ len--; }

                size_t start1 = 0;
                size_t len1 = seq1.size();
                while (qual1[start1] == '#'){ start1++; len1--; }
                while (qual1[start1 + len1 - 1] == '#'){ len1--; }

                seqs.push_back(seq.substr(start, len));
                seqs.push_back(seq1.substr(start1, len1));
                readn++;
                readNumber++;
            }
        }
        else {
            while (readn <= 300000 and (*in)) {
                getline(*in, title);
                getline(*in, seq);
                getline(*in, qualtitle);
                getline(*in, qual);

                size_t start = 0;
                size_t len = seq.size();
                while (qual[start] == '#'){ start++; len--; }
                while (qual[start + len - 1] == '#'){ len--; }

                seqs.push_back(seq.substr(start, len));
                readn++;
                readNumber++;
            }
        }
        cerr << "Buffered reading " << readn << "\t" << readNumber << endl;

        //
        // All done reading, release the thread lock.
        //
        sem_post(semreader);

        clock_t time2 = clock();
        if (ftype) {
            for (size_t seqi = 0; seqi < seqs.size()/2; ++seqi) {

                string seq = seqs[2*seqi];
                string seq1 = seqs[2*seqi+1];
                if (seq.size() < k or seq1.size() < k) { continue; }

                kmerCount_dict kmers, kmers1; 
                buildNuKmers(kmers, seq, k, 0);
                buildNuKmers(kmers1, seq1, k, 0);
                size_t ind, ind1, maxhit, maxhit1;
                tie(maxhit, ind) = countHit(kmers, kmerDBi, nloci);
                tie(maxhit1, ind1) = countHit(kmers1, kmerDBi, nloci);

                if (ind == ind1 and maxhit > threshold and maxhit1 > threshold) {
                    kmerCount_dict &query = queryResults[ind];
                    for (auto &p : kmers) {
                        if (query.count(p.first) == 1) {
                            query[p.first] += p.second;
                        }
                    }
                    for (auto &p : kmers1) {
                        if (query.count(p.first) == 1) {
                            query[p.first] += p.second;
                        }
                    }
                }
            }
        }
        else {
            for (size_t seqi = 0; seqi < seqs.size(); ++seqi) {

                string seq = seqs[seqi];
                if (seq.size() < k) { continue; }

                kmerCount_dict kmers;
                buildNuKmers(kmers, seq, k, 0);
                size_t ind, maxhit;
                tie(maxhit, ind) = countHit(kmers, kmerDBi, nloci);

                if (maxhit < threshold) { continue; }
                else {
                    kmerCount_dict &query = queryResults[ind];
                    for (auto &p : kmers) {
                        if (query.count(p.first) == 1) {
                            query[p.first] += p.second;
                        }
                    }
                }
            }
        }
        cout << "Batch query in " << (float)(clock() - time2)/CLOCKS_PER_SEC << " sec." << endl;
        if ((*in).good() == false or (qual == "")) {
            cout << "Finished at read index " << readNumber << endl;
            return;
        }
    }
}

int main(int argc, char* argv[]) {

    if (argc < 4) {
        cout << endl;
        cout << "Usage: nuQueryFasta -k <kmerSize> -q <file.queries> <-fs <file.fastq> | -fi <interleaved.fastq>> -o <outputFile> -p <nproc> -th <threshold>" << endl;
        cout << "  e.g. zcat ERR899717_1.fastq.gz | nuQueryFasta -k 21 -q PanGenomeGenotyping.21.kmers /dev/stdin ERR899717_1.fastq.21.kmers 8 5" << endl;
        cout << "  e.g. paste <(zcat ERR899717_1.fastq.gz | paste - - - -) <(zcat ERR899717_2.fastq.gz | paste - - - -) | tr '\\t' '\\n' | \\ \n";
        cout << "       nuQueryFasta -k 21 -q <*.kmers> -fi /dev/stdin -o <*.kmers> -p 8 -th 20" << endl;
        cout << "Options:" << endl;
        cout << "  -q     *.kmer file to be queried" << endl;
        cout << "  -fs    single end fastq file" << endl;
        cout << "  -fi    interleaved pair-end fastq file" << endl;
        cout << "  -p     Use n threads." << endl;
        cout << "  -th    Discard reads with maxhit below this threshold" << endl << endl;
        exit(0);
    }
   
    vector<string> args(argv, argv+argc);
    vector<string>::iterator it_k = find(args.begin(), args.begin()+argc, "-k") + 1;
    vector<string>::iterator it_q = find(args.begin(), args.begin()+argc, "-q") + 1;
    size_t ind_fs = distance(args.begin(), find(args.begin(), args.begin()+argc, "-fs"));
    size_t ind_fi = distance(args.begin(), find(args.begin(), args.begin()+argc, "-fi"));
    vector<string>::iterator it_o = find(args.begin(), args.begin()+argc, "-o") + 1;
    vector<string>::iterator it_p = find(args.begin(), args.begin()+argc, "-p") + 1;
    vector<string>::iterator it_th = find(args.begin(), args.begin()+argc, "-th") + 1;

    size_t k = stoi(*it_k);
    bool ftype;
    size_t ind_f = min(ind_fs, ind_fi) + 1;
    if (ind_f == ind_fs) { ftype = 0; }
    else { ftype = 1; }

    ifstream queryFile(*it_q);
    ifstream fastqFile(args[ind_f]);
    ofstream outFile(*it_o);
    assert(queryFile);
    assert(fastqFile);

    size_t nproc = stoi(*it_p);
    size_t threshold = stoi(*it_th);

    cout << "k: " << k << endl;
    cout << "ftype: " << ftype << endl;
    cout << "query: " << *it_q << endl;
    cout << "fastq: " << args[ind_f] << endl;
    
    // count the number of loci in a file
    cout << "total number of loci: ";
    clock_t time1 = clock();
    assert(queryFile);
    size_t nloci = 0;
    string line;
    while (getline(queryFile, line)) {
        if (line[0] == '>'){
            nloci++;
        }
    }
    cout << nloci << endl;
    queryFile.clear();
    queryFile.seekg(0, queryFile.beg);
 
    // read kmer info
    size_t ind = 0;
    kmerCount_dict kmers;
    vector<kmerCount_dict> kmerDB(nloci);
    kmerIndex_dict kmerDBi;
    getline(queryFile, line);
    while (true){
        if (queryFile.peek() == EOF or queryFile.peek() == '>'){
            if (kmers.size() != 0){
                kmerDB[ind] = kmers;
                kmers.clear();
            }
            ind++;
            if (queryFile.peek() == EOF){
                queryFile.close();
                break;
            }
            else {
                getline(queryFile, line);
            }
        }
        else {
            getline(queryFile, line, '\t');
            size_t kmer = stoul(line);
            kmers[kmer] = 0;
            kmerDBi[kmer].push_back(ind);
            getline(queryFile, line);
        }
    }
    cout << "read .kmer file in " << (float)(clock() - time1)/CLOCKS_PER_SEC << " sec." << endl;


    // create data for each process
    cout << "create data for each process..." << endl;
    time1 = clock();
    Threads threaddata(nproc, nloci);
    cout << "initialization" << endl;
    for (size_t i = 0; i < nproc; i++) {
        Counts &counts = threaddata.counts[i];
        
        //vector<kmerCount_dict> queryResults(nloci);
        for (size_t j = 0; j < nloci; j++) {
            counts.queryResults[j] = kmerDB[j];
        }
        counts.in = &fastqFile;
        counts.ftype = ftype;
        counts.kmerDBi = &kmerDBi;
        counts.readIndex = &readNumber;
        counts.threadIndex = i;
        counts.k = k;
        counts.threshold = threshold;
        cout << "thread " << i << " done" << endl;
    }
    kmerDB.clear();
    cout << "thread data preparation completed in " << (float)(clock() - time1)/CLOCKS_PER_SEC << " sec." << endl;

    const int idLen=10;
    char id[idLen+1];
    id[idLen] = '\0';
    srand (time(NULL));
    rand_str(id, idLen);

    string readerName = string("/semreader_") + string(id);
    string countName  = string("/semcount_") + string(id);
    string semfastqwriterName = string("/semfastqwriter_") + string(id);

    semreader = sem_open(readerName.c_str(), O_CREAT, 0644, 1);
    if (semreader == NULL) {
        cout << "ERROR opening semaphore. ERRNO " << errno << " " << readerName << endl;
        exit(1);
    }
    semcount = sem_open(countName.c_str(), O_CREAT, 0644, 1);
    if (semreader == NULL) {
        cout << "ERROR opening semaphore. ERRNO " << errno << " " << countName << endl;
        exit(1);
    }
    semfastqwriter = sem_open(semfastqwriterName.c_str(), O_CREAT, 0644, 1);
    if (semfastqwriter == NULL) {
        cout << "ERROR opening semaphore. ERRNO " << errno << " " << semfastqwriterName << endl;
        exit(1);
    }

    cout << "initializing semaphore..." << endl;
    sem_init(semreader, 1, 1);
    sem_init(semcount, 1, 1);
    sem_init(semcount, 1, 1);

    pthread_attr_t *threadAttr = new pthread_attr_t[nproc];
    int t;	

    for (t = 0; t < nproc; t++ ){
        pthread_attr_init(&threadAttr[t]);
    }
    pthread_t *threads = new pthread_t[nproc];

    for (t = 0; t < nproc; t++) {
        cout << "starting thread " << t << endl;
        pthread_create(&threads[t], &threadAttr[t], (void* (*)(void*))CountWords, &threaddata.counts[t]);
    }

    for (t = 0; t < nproc; t++) {
        pthread_join(threads[t], NULL);
    }

    cout << "combining restuls..." << endl;
    vector<kmerCount_dict> combinedQueryResults(nloci);
    for (size_t i = 0; i < threaddata.counts.size(); i++) {
        Counts &counts = threaddata.counts[i];
        for (size_t locus = 0; locus < counts.queryResults.size(); locus++){
            for (auto &p : counts.queryResults[locus]){
                combinedQueryResults[locus][p.first] += p.second;
            }
        }
    }

    cout << "writing outputs..." << endl;
    for (size_t locus = 0; locus < combinedQueryResults.size(); locus++){
        outFile << ">locus " << locus << '\n';
        //fprintf(outFile, ">locus %zu\n", locus);
        for (auto &p : combinedQueryResults[locus]){
            outFile << p.first << '\t' << p.second << '\n';
            //fprintf(outFile, "%zu\t%zu\n", p.first, p.second);
        }
    }

    
    outFile.close();
    return 0;
}


